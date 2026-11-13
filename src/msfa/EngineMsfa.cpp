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

//using namespace std;

#include "synth.h"
#include "exp2.h"
#include "fm_op_kernel.h"
#include "EngineMsfa.h"
#include "sin.h"
#include "../itcm.h"

// DX7 feedback through more than one operator (algorithm 4: OP6->OP5->OP4
// back to OP6; algorithm 6: OP6->OP5 back to OP6). Upstream msfa only ever
// fed the FB_IN operator back onto itself, so 4 sounded like 3 and 6 like
// 5. A loop can't be rendered one operator per block -- each sample of the
// first operator needs the previous sample of the last -- so the whole
// chain runs together here, one sample at a time, the way the DX7 itself
// computes it. The feedback term is the same two-sample average of the
// loop's last output that compute_fb() uses for self-feedback, and each
// operator modulates the next exactly as compute() does (phase + input).
DSY_ITCM_TEXT
static void compute_fb_chain(int32_t *output, FmOpParams *params, int first, int count,
                             const int32_t *gain1, const int32_t *gain2,
                             int32_t *fb_buf, int32_t fb_shift, bool add) {
  int32_t gain[3], dgain[3], phase[3], freq[3];
  for (int k = 0; k < count; k++) {
    gain[k] = gain1[k];
    dgain[k] = (gain2[k] - gain1[k] + (_N_ >> 1)) >> LG_N;
    phase[k] = params[first + k].phase;
    freq[k] = params[first + k].freq;
  }
  int32_t y0 = fb_buf[0];
  int32_t y = fb_buf[1];
  for (int i = 0; i < _N_; i++) {
    int32_t x = fb_shift < 16 ? (y0 + y) >> (fb_shift + 1) : 0;
    for (int k = 0; k < count; k++) {
      gain[k] += dgain[k];
      int32_t s = Sin::lookup(phase[k] + x);
      x = ((int64_t)s * (int64_t)gain[k]) >> 24;
      phase[k] += freq[k];
    }
    y0 = y;
    y = x;
    output[i] = add ? output[i] + x : x;
  }
  fb_buf[0] = y0;
  fb_buf[1] = y;
}

// Called once per active voice per quantum from DexedSynth::RenderQuantum()
// (dexed_synth.cpp, itself DSY_ITCM_TEXT) -- the real per-operator FM render
// dispatch, so this needs to live in ITCM too, not get called out to
// QSPIFLASH from an ITCM caller every single quantum. See itcm.h's own
// comment.
DSY_ITCM_TEXT
void EngineMsfa::render(int32_t *output, FmOpParams *params, int32_t algorithm, int32_t *fb_buf, int32_t feedback_shift) {
  const int32_t kLevelThresh = 1120;
  const FmAlgorithm alg = algorithms[algorithm];
  bool has_contents[3] = { true, false, false };

  // Multi-operator feedback loop, if this algorithm has one: fb_first
  // has FB_IN+FB_OUT, fb_last (later in the chain) FB_OUT alone.
  int fb_first = -1, fb_last = -1;
  for (int op = 0; op < 6; op++) {
    int32_t fb = alg.ops[op] & 0xc0;
    if (fb == 0xc0) fb_first = op;
    else if (fb == 0x80 && fb_first >= 0) fb_last = op;
  }
  if (fb_last - fb_first > 2) fb_last = -1; // only 4 and 6 exist (2-3 ops)

  for (uint8_t op = 0; op < 6; op++) {
    if (fb_last > 0 && op == fb_first) {
      int count = fb_last - fb_first + 1;
      int32_t g1[3], g2[3];
      bool active = false;
      for (int k = 0; k < count; k++) {
        FmOpParams &p = params[fb_first + k];
        g1[k] = p.gain_out;
        g2[k] = Exp2::lookup(p.level_in - (14 * (1 << 24)));
        p.gain_out = g2[k];
        active = active || g1[k] >= kLevelThresh || g2[k] >= kLevelThresh;
      }
      int32_t flags = alg.ops[fb_last];
      int32_t outbus = flags & 3;
      int32_t *outptr = (outbus == 0) ? output : buf_[outbus - 1].get();
      bool add = (flags & OUT_BUS_ADD) != 0 && has_contents[outbus];
      if (active) {
        compute_fb_chain(outptr, params, fb_first, count, g1, g2, fb_buf, feedback_shift, add);
        has_contents[outbus] = true;
      } else if (!add) {
        has_contents[outbus] = false;
      }
      for (int k = 0; k < count; k++)
        params[fb_first + k].phase += params[fb_first + k].freq << LG_N;
      op = (uint8_t)fb_last;
      continue;
    }
    int32_t flags = alg.ops[op];
    bool add = (flags & OUT_BUS_ADD) != 0;
    FmOpParams &param = params[op];
    int32_t inbus = (flags >> 4) & 3;
    int32_t outbus = flags & 3;
    int32_t *outptr = (outbus == 0) ? output : buf_[outbus - 1].get();
    int32_t gain1 = param.gain_out;
    int32_t gain2 = Exp2::lookup(param.level_in - (14 * (1 << 24)));
    param.gain_out = gain2;

    if (gain1 >= kLevelThresh || gain2 >= kLevelThresh) {
      if (!has_contents[outbus]) {
        add = false;
      }
      if (inbus == 0 || !has_contents[inbus]) {
        if ((flags & 0xc0) == 0xc0 && feedback_shift < 16) {
          // cout << op << " fb " << inbus << outbus << add << endl;
          FmOpKernel::compute_fb(outptr, param.phase, param.freq,
                                 gain1, gain2,
                                 fb_buf, feedback_shift, add);
        } else {
          // cout << op << " pure " << inbus << outbus << add << endl;
          FmOpKernel::compute_pure(outptr, param.phase, param.freq,
                                   gain1, gain2, add);
        }
      } else {
        // cout << op << " normal " << inbus << outbus << " " << param.freq << add << endl;
        FmOpKernel::compute(outptr, buf_[inbus - 1].get(),
                            param.phase, param.freq, gain1, gain2, add);
      }
      has_contents[outbus] = true;
    } else if (!add) {
      has_contents[outbus] = false;
    }
    param.phase += param.freq << LG_N;
  }
}
