#include "dexed_synth.h"
#include "fast_math.h"
#include "pan_law.h"
#include "svf_setup.h"
#include "itcm.h"
#include "dexed_sysex.h"
#include "msfa/exp2.h"
#include "msfa/sin.h"
#include "msfa/freqlut.h"
#include "msfa/env.h"
#include "msfa/pitchenv.h"
#include "msfa/porta.h"
#include <cmath>
#include <cstring>
#include <cstdio>

namespace
{
// Q24 fixed-point -- msfa/sin.cpp builds its lookup table to a full-scale
// sine amplitude of exactly 1<<24 (see its own table-fill code: a 1<<30
// scale right-shifted by 6), and fm_op_kernel.cpp's compute()/
// compute_pure() apply per-operator gain as a Q24 multiply-then->>24.
// Derived directly from the Apache-2.0 msfa source this project vendors
// (sin.cpp/fm_op_kernel.cpp), not from dexed.cpp's own (excluded) code.
//
// A single Q24 unit alone is NOT actual full-scale for a real rendered
// note, though -- msfa's internal envelope/gain-staging deliberately
// carries headroom well above nominal unity (the same "room for FM
// modulation to push past simple +-1 waveshaping" design every fixed-
// point FM engine like this needs). Measured directly on real hardware
// (temporary peak-diagnostic instrumentation, since removed) against
// this project's own worst-case 6-fully-additive-carrier test patch: a
// single note peaked at ~1.74x this naive Q24-only scale, a 3-note
// chord ~4.41x.
//
// An initial kHeadroomScale of 1/4.41 (landing that 3-note chord right
// at the edge of unity) turned out to leave no real margin once real
// factory patches (this project's own hand-tuned test patch was, if
// anything, unusually mild -- many real patches use feedback, which
// self-modulates far harder) and real polyphonic chords beyond 3 notes
// entered the picture: tanhf()'s own soft limiter in Process() was
// routinely engaging hard enough to audibly compress/distort a plain
// single real patch playing alone, not just catching rare outlier
// peaks the way it's meant to. Doubled again here for real margin --
// tanhf() should stay a rare safety net for genuine outliers (bigger
// chords, unusual phase alignment, hotter-than-average patches), not
// something routine playing runs into; overall loudness is what the
// master volume knob (already reaching up to 1.43x at 100%) is for.
//
// Even doubled, this fixed scale alone still isn't enough margin for
// the hottest real factory patches (confirmed on a real ROM1A patch
// using max feedback=7 and near-unity output levels on every operator
// -- fine alone, audibly compressing tanhf() once a second note
// stacked on top). A per-block voice-count compensation on top of this
// fixed scale was tried and removed -- a real, measured artifact: it
// keyed off currently-HELD notes, not currently-SOUNDING voices, so
// releasing one note of a chord made the others audibly louder
// mid-release-tail, before the released note had actually finished
// sounding. So this fixed scale is doubled again here instead
// -- matching the reference implementation's own approach (positionhigh/
// MicroDexed-touch's Synth_Dexed: a single fixed headroom margin, no
// per-chord dynamic compensation at all) rather than trying to fix the
// compensation path's own release-tail bug. Trade-off: single notes
// are now quieter too, not just chords, since this is a flat scale with
// no way to apply more headroom only when it's actually needed --
// compensate with the Output Level knob if needed (it sits after this
// scale and before the live/record split point, same as this scale
// itself -- so it compensates BOTH live monitoring and what gets
// recorded into a layer equally, not just one or the other).
// Doubled again (1/17.64 -> 1/35.28) after a real chord still clipping
// at Output Level 50% -- confirmed that knob sits entirely after this
// point and after the limiter, so it could never have reduced how hard
// the limiter was compressing in the first place; only this constant
// actually does that.
constexpr float kQ24Scale      = 1.f / (float)(1 << 24);
constexpr float kHeadroomScale = 1.f / 35.28f;

// Per-voice safety clamp, applied in RenderQuantum() to each voice's own
// RAW (pre-kQ24Scale) int32 output before it's summed into the shared
// float accumulator -- see voice_scratch_'s own comment in
// dexed_synth.h for why this needs to happen per-voice, at the source,
// not later on the already-summed result. 64x the nominal (1<<24)
// unity scale: real measured peaks top out around 1.74x for a single
// voice and 4.41x for a 3-note chord (see kHeadroomScale's own comment
// above), so this is ~15x more headroom than the hottest real patch
// measured so far ever reached -- comfortably clear of any legitimate
// signal, while still well short of actual int32_t wraparound (~2^31,
// i.e. ~128x this same nominal scale), which is the specific failure
// mode this exists to prevent.
constexpr int32_t kVoiceClampQ24 = (int32_t)1 << 30;
static_assert(kVoiceClampQ24 == (int32_t)1 << (31 - 1), "RenderQuantum()'s __SSAT(x, 31) is this clamp");

// Wavefolder -- Dexed-only effect (see SetFoldAmount01()'s own comment).
// This insertion point (right after kHeadroomScale, see its own comment
// above) is deliberately the quietest point in this engine's whole
// chain -- a single voice's real measured peak there is ~0.05, a 3-note
// chord ~0.125 -- so, same reasoning the old (removed) Drive feature's
// kDriveMaxGain=60 needed at this identical point, real gain is needed
// to push a typical signal into the fold's own "interesting" range at
// all. Started lower than that 60x given a real hot/feedback-heavy
// patch was already distorting at just 6% knob position with the old
// feature's 60x -- retune by ear on real hardware, not a final value.
constexpr float kFoldMaxGain = 40.f;

// See DexedSynth::IsLimiting()'s own comment. 1.0 matches this engine's
// own documented limiter behavior -- tanhf() only compresses hard once
// a peak actually approaches/exceeds unity, so that's the meaningful
// threshold for "the limiter is doing real work," not some fraction of
// the bounded post-tanhf() output. 0.5s hold, same convention as every
// other clip latch in this project (LooperLayer::clip_hold_, main.cpp's
// own meters) -- long enough that a one-sample peak between two ~30Hz
// UI redraws still has a real chance of being seen.
constexpr float kPreLimiterClipThreshold = 1.f;
constexpr float kClipHoldSeconds         = 0.5f;
// Same peak-meter release shape as main.cpp's own kMeterReleaseSeconds
// (see its own comment there) -- instant attack, 300ms release, so
// GetPreLimiterLevel01() actually tracks a visible peak instead of a
// symmetric follower that decays back to nothing in ~0.27ms @ 48kHz.
constexpr float kMeterReleaseSeconds = 0.3f;
} // namespace

void DexedSynth::Init(float sample_rate)
{
    sample_rate_ = sample_rate;

    // One-time msfa engine setup -- every one of these is idempotent
    // (each guards itself with its own static initDone flag), so calling
    // this more than once is harmless, but Init() is only ever called
    // once from main() in practice.
    Exp2::init();
    Sin::init();
    FmCore::init();
    Freqlut::init(sample_rate_);
    Env::init_sr(sample_rate_);
    PitchEnv::init(sample_rate_);
    Porta::init_sr(sample_rate_);
    Lfo::init(sample_rate_);

    ctrls_.core          = &engine_;
    ctrls_.values_[kControllerPitch]        = 0x2000; // centered, see Dx7Note::compute()'s pb math
    ctrls_.values_[kControllerPitchRange]   = 2; // +-2 semitones, a reasonable default
    ctrls_.values_[kControllerPitchStep]    = 0; // continuous bend, not stepped
    ctrls_.values_[kControllerPortamentoGlissando] = 0;
    ctrls_.masterTune    = 0;
    ctrls_.opSwitch      = 0x3f; // all 6 operators enabled
    ctrls_.aftertouch_cc = 0;
    ctrls_.breath_cc     = 0;
    ctrls_.foot_cc       = 0;
    ctrls_.modwheel_cc   = 0;
    ctrls_.portamento_enable_cc = false;
    ctrls_.portamento_cc        = 0;
    ctrls_.portamento_gliss_cc  = false;
    // Mod wheel -> vibrato (pitch), the standard default routing real
    // DX7 hardware/most patches use -- Controllers::wheel (an FmMod) is
    // otherwise default-constructed with range 0 and every target
    // false, i.e. completely disabled regardless of modwheel_cc's own
    // value, which is a genuinely separate, independent setting from
    // the patch's own pitch-mod-sensitivity byte (see patch_[143]
    // below) -- both have to be configured for the wheel to actually
    // move anything.
    ctrls_.wheel.setRange(50);
    ApplyModWheelTarget(); // defaults to Pitch (bit0) -- same as the old hardcoded setTarget(1)
    ctrls_.refresh();

    // LFO pitch mod SENSITIVITY note: despite the name, patch byte 143
    // is the note's overall susceptibility to ANY pitch modulation
    // source (both the LFO's own depth AND the mod wheel's own
    // contribution via ctrls_.wheel above -- see Dx7Note::compute()'s
    // senslfo, which gates both through the same multiply) -- a patch
    // with this at 0 will not respond to the mod wheel at all
    // regardless of ctrls_.wheel's own routing, same as every other
    // real DX7 patch parameter (nothing special is done here to force
    // it on for factory/user patches that were authored with it off).

    // Boot default: real factory preset 0 (first patch in the first
    // category), same "always exactly factory preset 0, no separately
    // hand-coded default to drift out of sync" convention the removed
    // FmSynth::Init() used. lfo_.reset() happens inside ApplyPreset()'s
    // own SetPatch() call below.
    ApplyPreset(GetFactoryPreset(0));

    // Every voice's Dx7Note gets a real init() up front -- RenderQuantum()
    // below calls compute() on every voice slot unconditionally every
    // quantum, even ones never yet NoteOn()'d, and a Dx7Note that's never
    // had init() called has an uninitialized algorithm_ index (undefined
    // behavior the moment render() looks it up in fm_core.cpp's
    // algorithms[32] table) -- this is what keeps that always-safe rather
    // than relying on every call site remembering to check first.
    //
    // Pre-warmed against an all-zero SILENT patch, not patch_ itself at
    // velocity 0 -- Env::init()'s own attack always begins ramping toward
    // the patch's outlevel the instant init() runs regardless of
    // velocity, and per-operator velocity sensitivity is 0 in the test
    // patch above, so ScaleVelocity(0, 0) == ScaleVelocity(127, 0) == 0:
    // velocity has NO attenuating effect at all with this patch, and
    // "silent because velocity 0" was a real bug -- every pre-warmed
    // voice rang out at full volume from the moment of boot. An all-
    // zero patch's outlevel is genuinely 0 for every operator regardless
    // of velocity, so this is silent (and, via msfa's own kLevelThresh
    // gate in EngineMsfa::render(), cheap) no matter what the real patch
    // above does with velocity sensitivity.
    uint8_t silent_patch[156];
    std::memset(silent_patch, 0, sizeof(silent_patch));
    for(int i = 0; i < kVoiceSlots; i++)
        voices_[i].note.init(silent_patch, 60, 0, 60, -1, &ctrls_);
}

void DexedBus::SetOutputLevel01(float v01)
{
    output_level01_ = v01 < 0.f ? 0.f : (v01 > 1.f ? 1.f : v01);
    // Same curve every other engine's own Output Level uses (see
    // GranularEngine::SetOutputLevel01()), so this instrument's level
    // knob feels consistent with the rest of the mixer.
    output_level_ = powf(output_level01_, 2.5f) * 1.4f;
}

void DexedBus::SetPan01(float v01)
{
    pan01_      = v01 < 0.f ? 0.f : (v01 > 1.f ? 1.f : v01);
    EqualPowerPan(pan01_, &pan_l_gain_, &pan_r_gain_);
}

void DexedBus::SetFoldAmount01(float v01)
{
    fold_amount01_ = v01 < 0.f ? 0.f : (v01 > 1.f ? 1.f : v01);
    // amount01=0 -> gain=1.0, but Process() skips the fold altogether
    // at 0%: Wavefolder at gain 1.0 still folds anything past +/-1.
    fold_.SetGain(1.f + fold_amount01_ * (kFoldMaxGain - 1.f));
}

void DexedBus::SetFoldTrim01(float v01)
{
    fold_trim01_ = v01 < 0.f ? 0.f : (v01 > 1.f ? 1.f : v01);
    // Deliberately NOT the *1.4f boosted-ceiling convention
    // SetOutputLevel01() above uses -- this trim's default (1.0) has
    // to mean exactly "unity, no change" (see fold_trim01's own doc
    // comment in DexedPresetData), so v01=1.0 must land at gain 1.0,
    // not a +40% boost.
    fold_trim_ = powf(fold_trim01_, 2.5f);
}

// Called from RenderQuantum() (ITCM) for every non-held voice slot, every
// quantum -- a real, standalone (confirmed in the link map) miss, same
// pattern as everywhere else. See itcm.h's own comment.
//
// A released voice also counts as idle once every operator that reaches
// the output (a carrier) has fallen below kAudibleGain, about -66 dB of an
// operator's full scale -- far below anything audible in a mix. Waiting
// instead for every envelope, modulators included, to run all the way down
// (msfa's own floor is ~-83 dB) kept long-release patches computing whole
// silent voices for seconds after the last note.
namespace
{
constexpr uint32_t kAudibleGain = 8400; // operator gains are Q24 (full scale 1 << 24)
}

DSY_ITCM_TEXT
bool DexedSynth::VoiceIsIdle(int voice_index)
{
    if(!voices_[voice_index].played)
        return true;
    VoiceStatus st;
    voices_[voice_index].note.peekVoiceStatus(st);
    bool ended = true;
    for(int op = 0; op < 6; op++)
        if(st.ampStep[op] < 4)
            ended = false;
    if(ended)
        return true;
    if(voices_[voice_index].held_note != -1)
        return false;
    // Released: done once every carrier is in its release (or finished)
    // and below kAudibleGain.
    const uint8_t carriers = FmCore::get_carrier_operators(patch_[134] & 31);
    for(int op = 0; op < 6; op++)
        if((carriers & (1 << op)) && (st.ampStep[op] < 3 || st.amp[op] >= kAudibleGain))
            return false;
    return true;
}

int DexedSynth::FindVoiceForNote(uint8_t note, int* same_key)
{
    // Only the first max_voices_ slots are handed out; the rest just
    // finish any release tail they already had (RenderQuantum() still
    // walks every slot).
    //
    // A key struck again while its note still sounds -- held, under the
    // sustain pedal, or in its release -- lets that voice go on into its
    // own release and takes a fresh one, as Dexed does, which NoteOn()
    // starts in phase with it. Tried before this, each clicking on soft
    // re-strikes: restarting the same voice from zero under a 5 ms fade
    // (a dip); a fresh voice at whatever phase it had (the two same-pitch
    // sines suddenly adding or cancelling); and restarting the same voice
    // from its current level (a full-volume attack burst on slow,
    // feedback-heavy sounds like RECORDER).
    *same_key = -1;
    for(int i = 0; i < max_voices_; i++)
        if(voices_[i].sounding_note == (int)note && (voices_[i].held_note == (int)note || !VoiceIsIdle(i)))
        {
            *same_key = i;
            if(voices_[i].held_note != -1)
            {
                __disable_irq(); // same race as NoteOff()
                voices_[i].note.keyup();
                voices_[i].held_note = -1;
                __enable_irq();
            }
        }

    int  best_free    = -1;
    bool best_is_idle = false;
    for(int i = 0; i < max_voices_; i++)
    {
        if(voices_[i].held_note != -1)
            continue;
        bool idle = VoiceIsIdle(i);
        if(best_free < 0 || (idle && !best_is_idle))
        {
            best_free    = i;
            best_is_idle = idle;
        }
    }
    if(best_free >= 0)
        return best_free;

    int oldest = 0;
    for(int i = 1; i < max_voices_; i++)
        if(voices_[i].triggered_at < voices_[oldest].triggered_at)
            oldest = i;
    return oldest;
}

void DexedSynth::NoteOn(uint8_t note, uint8_t velocity)
{
    int    same_key = -1;
    int    vi       = FindVoiceForNote(note, &same_key);
    Voice& v        = voices_[vi];

    // Stealing a slot that is still sounding (a voice steal, or a
    // releasing tail when no idle slot is left) would restart it from zero in
    // one sample -- an audible click, on every note at Voices = 1. Move
    // the old sound to an idle slot (slots above max_voices_, including
    // the kFadeSlots spares, never take new notes) and let RenderQuantum()
    // fade it out instead. If a burst of steals has every spare already
    // fading, take over the fade nearest its end -- it's almost silent,
    // so cutting it is inaudible where cutting a held note clicks.
    int fade_to = -1;
    if(!VoiceIsIdle(vi))
    {
        for(int i = kVoiceSlots - 1; i >= 0 && fade_to < 0; i--)
            if(i != vi && voices_[i].held_note == -1 && VoiceIsIdle(i))
                fade_to = i;
        if(fade_to < 0)
            for(int i = 0; i < kVoiceSlots; i++)
                if(i != vi && voices_[i].fade_left > 0
                   && (fade_to < 0 || voices_[i].fade_left < voices_[fade_to].fade_left))
                    fade_to = i;
    }

    // srcnote == note, porta = -1: no portamento transition (see
    // Dx7Note::init()'s own porta handling -- a real portamento path is
    // future incremental-editing scope, not this phase's). A fresh
    // init() (not update()) every time, whether this voice was free,
    // stolen, or already sounding this exact note -- unlike DaisySP's
    // Adsr::Retrigger() convenience the removed FmSynth used, Dx7Note's
    // own init() already gives a full fresh envelope attack on its own,
    // so no separate retrigger-vs-fresh-strike branch is needed here.
    //
    // Brief critical section around the actual mutation: this runs from
    // ControlTimerCallback (TIM5, a genuinely LOWER NVIC priority than
    // the audio DMA ISR -- see that function's own doc comment), and
    // Dx7Note::init() is a real multi-step write across several of this
    // voice's internal fields (env_[], basepitch_[], algorithm_, ...),
    // not an atomic flag set. RenderQuantum() (the audio ISR) reads this
    // exact same voice's state every quantum and, being higher priority,
    // can genuinely preempt this call mid-mutation -- real hardware
    // playtesting reported dropped/glitched notes specifically under
    // fast playing, consistent with landing in that window. Disabling
    // IRQs for this one voice's own init() call (a small, fixed amount
    // of work, never a loop over unbounded data) is enough to make it
    // atomic with respect to the audio ISR without meaningfully risking
    // that ISR's own timing.
    // TRANSPOSE (patch byte 144, 24 = middle C plays C3) moves the
    // pitch only -- held_note stays the key actually pressed, so
    // NoteOff() still finds it. LFO KEY SYNC/LFO DELAY restart and OSC
    // KEY SYNC both happen on every key, same as Dexed's own keydown.
    int played = TransposedNote(note);
    __disable_irq();
    if(fade_to >= 0)
    {
        voices_[fade_to]           = v;
        voices_[fade_to].held_note = -1;
        voices_[fade_to].sounding_note = -1; // a fading copy is never re-struck
        voices_[fade_to].fade_left = kStealFadeSamples;
    }
    v.fade_left = 0;
    lfo_.keydown();
    v.note.init(patch_, played, velocity, played, -1, &ctrls_);
    // OSC KEY SYNC on: every note starts at phase 0, as the patch asks --
    // its attack is voiced for that (often with heavy feedback), and
    // starting in step with the old voice instead made it harsher. Off:
    // the oscillators run free, so the new voice starts in step with the
    // one still sounding this key, and the two same-pitch sines add
    // instead of suddenly cancelling (RHODES 8). Skipped when that voice
    // is the one being reused: its phases are already these.
    if(patch_[136])
        v.note.oscSync();
    else if(same_key >= 0 && same_key != vi)
        v.note.transferPhase(voices_[same_key].note);
    v.played = true;
    v.held_note    = note;
    v.sounding_note = note;
    v.velocity     = velocity;
    v.triggered_at = ++trigger_seq_;
    __enable_irq();
}

void DexedSynth::NoteOff(uint8_t note)
{
    for(int i = 0; i < kVoiceSlots; i++)
    {
        if(voices_[i].held_note == (int)note)
        {
            // Same race as NoteOn()'s own -- see its comment.
            // Dx7Note::keyup() also writes multiple operators' env_[]
            // state, not a single atomic flag.
            __disable_irq();
            voices_[i].note.keyup();
            // Freed immediately (see NoteOn()'s own free-voice-
            // preference logic) -- the voice keeps rendering its own
            // real release tail regardless (see RenderQuantum(), which
            // computes every voice unconditionally).
            voices_[i].held_note = -1;
            __enable_irq();
        }
    }
}

void DexedSynth::SetMaxVoices(int n)
{
    max_voices_ = n < 1 ? 1 : (n > kMaxVoices ? kMaxVoices : n);
    // Same keyup-and-free as NoteOff() for every held slot now above the
    // limit -- same IRQ guard, see NoteOn()'s comment.
    for(int i = max_voices_; i < kVoiceSlots; i++)
    {
        if(voices_[i].held_note == -1)
            continue;
        __disable_irq();
        voices_[i].note.keyup();
        voices_[i].held_note = -1;
        __enable_irq();
    }
}

void DexedSynth::AllNotesOff()
{
    for(int i = 0; i < kVoiceSlots; i++)
    {
        if(voices_[i].held_note == -1)
            continue;
        __disable_irq();
        voices_[i].note.keyup();
        voices_[i].held_note = -1;
        __enable_irq();
    }
}

DSY_ITCM_TEXT
void DexedSynth::RenderQuantum()
{
    std::memset(stage_f_, 0, sizeof(stage_f_));

    ctrls_.refresh();
    int32_t lfo_val   = lfo_.getsample();
    int32_t lfo_delay = lfo_.getdelay();

    // Every voice NOT already confirmed idle -- used to unconditionally
    // compute() every one of the voice slots every block on the
    // theory that msfa's own internal gain threshold made a silent
    // voice already cheap, but that was never actually true: msfa runs
    // the full per-operator pitch/amp-mod/envelope math (see
    // Dx7Note::compute()) regardless of whether a voice is sounding --
    // there's no early-exit in there at all. A played note typically
    // uses only a few of the slots at once, so computing
    // all of them unconditionally meant paying for several silent voices'
    // full FM synthesis on every single block for nothing. VoiceIsIdle()
    // (see its own comment -- already trusted elsewhere in this file for
    // voice-stealing allocation) checks the real per-operator envelope
    // state, so skipping compute() once a free voice is confirmed idle
    // is provably silent already, not a guess -- a NEW NoteOn always
    // re-triggers whichever voice it allocates, so a skipped voice can
    // never get stuck silent when it shouldn't be.
    //
    // Each computed voice renders into its own freshly-zeroed
    // voice_scratch_, gets clamped (kVoiceClampQ24) and converted to
    // float, THEN summed into stage_f_ -- see voice_scratch_'s own doc
    // comment in dexed_synth.h for why every voice previously sharing
    // one raw int32_t accumulator (no clamp at all) was a real bug, not
    // just a style choice, and matches how the reference implementation
    // (positionhigh/MicroDexed-touch's Synth_Dexed) does this same step.
    for(int i = 0; i < kVoiceSlots; i++)
    {
        if(voices_[i].held_note != -1)
            ; // held: always computed
        else if(VoiceIsIdle(i))
        {
            // Confirmed silent -- nothing to compute or add. Marked
            // unplayed too, so from now on VoiceIsIdle() answers on its
            // first line instead of reading all six envelopes again
            // every block until this slot's next NoteOn().
            voices_[i].played    = false;
            voices_[i].fade_left = 0;
            continue;
        }

        std::memset(voice_scratch_, 0, sizeof(voice_scratch_));
        voices_[i].note.compute(voice_scratch_, lfo_val, lfo_delay, &ctrls_);

        // Summed still in Q24 units: kQ24Scale (a power of two, so
        // exact either way) is applied once in Render() instead of per
        // voice. __SSAT(raw, 31) is the kVoiceClampQ24 clamp in one
        // instruction.
        int fade = voices_[i].fade_left;
        if(fade == 0)
        {
            for(int j = 0; j < kMsfaBlock; j++)
                stage_f_[j] += (float)__SSAT(voice_scratch_[j], 31);
        }
        else
        {
            for(int j = 0; j < kMsfaBlock; j++)
            {
                stage_f_[j] += (float)__SSAT(voice_scratch_[j], 31) * ((float)fade * kStealFadeInv);
                if(fade > 0)
                    fade--;
            }
            // Faded out: mark the slot unplayed so VoiceIsIdle() skips
            // it from now on (its envelope may still be mid-note).
            voices_[i].fade_left = fade;
            if(fade == 0)
                voices_[i].played = false;
        }
    }

    stage_pos_ = 0;
}

DSY_ITCM_TEXT
bool DexedSynth::Render(size_t size, float* out_l, float* out_r)
{
    // Nothing held and nothing still ringing: add nothing. The bus keeps
    // running its own tail (filter, fold) until that has died away too.
    {
        bool any_voice = false;
        for(int vi = 0; vi < kVoiceSlots && !any_voice; vi++)
            any_voice = voices_[vi].held_note != -1 || !VoiceIsIdle(vi);
        if(!any_voice)
            return false;
    }

    // stage_f_ is in Q24 units (see RenderQuantum()); both fixed scales
    // and this engine's Level and Pan in one multiply per channel.
    const float gl = kQ24Scale * kHeadroomScale * engine_level_ * bal_l_;
    const float gr = kQ24Scale * kHeadroomScale * engine_level_ * bal_r_;
    for(size_t i = 0; i < size; i++)
    {
        if(stage_pos_ >= kMsfaBlock)
            RenderQuantum();

        float s = stage_f_[stage_pos_];
        stage_pos_++;
        out_l[i] += s * gl;
        out_r[i] += s * gr;
    }
    return true;
}

void DexedSynth::SetEngineLevel01(float v01)
{
    engine_level01_ = v01 < 0.f ? 0.f : (v01 > 1.f ? 1.f : v01);
    // Same curve as the bus Level, without its 1.4x ceiling, so 100% is
    // exactly unity.
    engine_level_ = powf(engine_level01_, 2.5f);
}

void DexedSynth::SetEnginePan01(float v01)
{
    engine_pan01_ = v01 < 0.f ? 0.f : (v01 > 1.f ? 1.f : v01);
    // Balance, not equal-power: the bus applies the equal-power Pan
    // after the limiter, as Dexed always did, and a second -3 dB in the
    // centre here would change how hard a lone engine drives it.
    bal_l_ = engine_pan01_ <= 0.5f ? 1.f : 2.f * (1.f - engine_pan01_);
    bal_r_ = engine_pan01_ >= 0.5f ? 1.f : 2.f * engine_pan01_;
}

void DexedBus::Init(float sample_rate)
{
    sample_rate_ = sample_rate;
    filter_l_.Init(sample_rate_);
    filter_r_.Init(sample_rate_);
    fold_.Init();
    SetOutputLevel01(output_level01_);
    SetPan01(pan01_);
    SetFoldAmount01(fold_amount01_);
    SetFoldTrim01(fold_trim01_);
    SetGain01(gain01_);
}

DSY_ITCM_TEXT
void DexedBus::Process(size_t size, float* l, float* r, bool mono, bool active,
                       float* reverb_send_l, float* reverb_send_r, float* delay_send_l,
                       float* delay_send_r)
{
    // Per-block constants (were divisions every sample).
    const float meter_release = 1.f / (kMeterReleaseSeconds * sample_rate_);
    const float inv_sr        = 1.f / sample_rate_;

    // No engine added anything and nothing pending: skip all the
    // per-sample work (the same rule as GranularEngine::Process()).
    // Output is plain zero, the sends get nothing, and the meters decay
    // as they would have.
    if(!active && last_block_peak_ < 1e-6f)
    {
        for(size_t i = 0; i < size; i++)
            l[i] = r[i] = 0.f;
        pre_limiter_level_ -= pre_limiter_level_ * meter_release * (float)size;
        if(pre_limiter_level_ < 0.f)
            pre_limiter_level_ = 0.f;
        if(pre_limiter_clip_hold_ > 0.f)
            pre_limiter_clip_hold_ -= inv_sr * (float)size;
        return;
    }
    float block_peak = 0.f;
    // Block-rate filter cutoff/res -- same curve/guard every other
    // engine's own bus filter uses (see GranularEngine::Process()).
    if(filter_mode_ != FilterMode::Off)
    {
        float cutoff_hz = kFilterMinHz * powf(kFilterMaxHz / kFilterMinHz, filter_cutoff01_);
        float nyquist_guard = sample_rate_ / 3.f - 1.f;
        cutoff_hz = cutoff_hz < kFilterMinHz ? kFilterMinHz
                    : cutoff_hz > nyquist_guard ? nyquist_guard
                                                : cutoff_hz;
        filter_l_.SetFreq(cutoff_hz);
        filter_l_.SetRes(filter_res01_ * 0.9f);
        filter_r_.SetFreq(cutoff_hz);
        filter_r_.SetRes(filter_res01_ * 0.9f);
    }

    if(last_mono_ && !mono)
        filter_r_ = filter_l_;
    last_mono_ = mono;

    const float send       = reverb_send01_;
    const float delay_send = delay_send01_;
    const bool  fold_on    = fold_amount01_ > 0.f;
    const float drive      = fold_trim_ * gain_;
    for(size_t i = 0; i < size; i++)
    {
        float sl = l[i];
        // Mono path: both engines centred, so r[] is a copy of l[] --
        // one filter and one fold, exactly the old single-engine chain.
        float sr = mono ? sl : r[i];

        // Post-mix bus filter, same shape/position as GranularEngine's
        // own (before the soft limiter, so a resonant peak still gets
        // caught by it).
        if(filter_mode_ != FilterMode::Off)
        {
            filter_l_.Process(sl);
            if(!mono)
                filter_r_.Process(sr);
            switch(filter_mode_)
            {
                case FilterMode::LowPass:
                    sl = filter_l_.Low();
                    sr = mono ? sl : filter_r_.Low();
                    break;
                case FilterMode::HighPass:
                    sl = filter_l_.High();
                    sr = mono ? sl : filter_r_.High();
                    break;
                case FilterMode::BandPass:
                    sl = filter_l_.Band();
                    sr = mono ? sl : filter_r_.Band();
                    break;
                default: break;
            }
        }

        // Wavefolder -- after the bus filter, before the pre-limiter
        // tap just below, so IsLimiting()/GetPreLimiterLevel01() read
        // the post-fold signal. Skipped entirely at Fold 0%: gain 1.0 is
        // only unity for |s| < 1 -- past that the Wavefolder folds the
        // peak back down, so a loud chord got folded (a buzzy crackle
        // the clip meter, reading after the fold, never showed) instead
        // of reaching the FastTanh limiter below as intended.
        if(fold_on)
        {
            sl = fold_.Process(sl);
            sr = mono ? sl : fold_.Process(sr);
        }
        sl *= drive;
        sr *= drive;

        // See IsLimiting()'s/GetPreLimiterLevel01()'s own comments --
        // both read the PRE-limiter value specifically, since FastTanh()
        // below bounds its own output to (-1,1) no matter how hard
        // this is driving it, so either check on the result could
        // never usefully fire. Same instant-attack/slow-release peak
        // shape main.cpp's own meters use (see UpdateLevelMeter()).
        float pre_mag = fabsf(sl) > fabsf(sr) ? fabsf(sl) : fabsf(sr);
        if(pre_mag > pre_limiter_level_)
            pre_limiter_level_ = pre_mag;
        else
            pre_limiter_level_ -= pre_limiter_level_ * meter_release;
        if(pre_mag >= kPreLimiterClipThreshold)
            pre_limiter_clip_hold_ = kClipHoldSeconds;
        else if(pre_limiter_clip_hold_ > 0.f)
            pre_limiter_clip_hold_ -= inv_sr;

        // Soft limiter, on top of kHeadroomScale's own real-measured
        // correction -- catches genuinely rare peaks (bigger chords,
        // unusual phase alignment, or a resonant filter peak) so those
        // compress gracefully instead of hard-clipping. FastTanh() is
        // near-transparent for normal signal levels (FastTanh(0.5)
        // attenuates only ~8%) and only compresses hard once a peak
        // actually approaches/exceeds unity.
        sl = FastTanh(sl) * output_level_;
        sr = mono ? sl : FastTanh(sr) * output_level_;
        // Pan applied last, same position GranularEngine's own Process()
        // uses (post-limiter/output-level, feeding both the dry output
        // and the sends).
        sl *= pan_l_gain_;
        sr *= pan_r_gain_;
        float pk = fabsf(sl) > fabsf(sr) ? fabsf(sl) : fabsf(sr);
        if(pk > block_peak)
            block_peak = pk;
        l[i] = sl;
        r[i] = sr;
        if(send > 0.f)
        {
            reverb_send_l[i] += sl * send;
            reverb_send_r[i] += sr * send;
        }
        if(delay_send > 0.f)
        {
            delay_send_l[i] += sl * delay_send;
            delay_send_r[i] += sr * delay_send;
        }
    }
    last_block_peak_ = block_peak;
}

void DexedSynth::ApplyPatchToHeldVoices()
{
    // Called from the main loop (preset load/preview, macro knobs) --
    // also a lower NVIC priority than the audio ISR, same race as
    // NoteOn()/NoteOff()'s own (see NoteOn()'s comment). One short
    // critical section per voice rather than one covering the whole
    // loop, keeping each individual disable window as small as possible.
    for(int i = 0; i < kVoiceSlots; i++)
    {
        Voice& v = voices_[i];
        if(v.held_note != -1)
        {
            __disable_irq();
            v.note.update(patch_, TransposedNote(v.held_note), v.velocity, -1, &ctrls_);
            __enable_irq();
        }
    }
}

void DexedSynth::SetPatch(const uint8_t new_patch[156])
{
    std::memcpy(patch_, new_patch, 156);
    // A release level (EG L4) above the DX7's 99 -- found in banks from the
    // web, and a dozen factory voices -- converts to a target LOUDER than
    // full in Env::advance(), so the release rises instead of decaying and
    // the note never ends (reported: VRC110A's EP.SawBel). Treat it as 0,
    // silence, which is what these patches clearly meant. Every other
    // value is left exactly as stored.
    for(int op = 0; op < 6; op++)
        if(patch_[op * 21 + 7] > 99)
            patch_[op * 21 + 7] = 0;
    for(int i = 126; i <= 140; i++) // 137, LFO speed, comes back unchanged
        patch_[i] = SafePatchByte(i, patch_[i]);
    std::memcpy(patch_baseline_, patch_, 156);
    brightness01_ = 0.5f; // re-center -- "as stored" for the freshly loaded patch
    env_speed01_  = 0.5f;
    lfo_.reset(&patch_[137]);
    ApplyPatchToHeldVoices();
}

void DexedSynth::SetBrightness01(float v01)
{
    brightness01_ = v01 < 0.f ? 0.f : (v01 > 1.f ? 1.f : v01);
    // 0x at v01=0, 1x (unchanged) at v01=0.5, 2x at v01=1.
    float scale = brightness01_ >= 0.5f ? 1.f + (brightness01_ - 0.5f) * 2.f
                                          : brightness01_ * 2.f;

    uint8_t carrier_mask = FmCore::get_carrier_operators(patch_baseline_[134]);
    for(int op = 0; op < 6; op++)
    {
        if(carrier_mask & (1 << op))
            continue; // carriers untouched -- only modulators shape brightness
        int off        = op * 21;
        int base_level = patch_baseline_[off + 16];
        int new_level  = (int)((float)base_level * scale + 0.5f);
        new_level      = new_level < 0 ? 0 : (new_level > 99 ? 99 : new_level);
        patch_[off + 16] = (uint8_t)new_level;
    }
    ApplyPatchToHeldVoices();
}

void DexedSynth::SetEnvSpeed01(float v01)
{
    env_speed01_ = v01 < 0.f ? 0.f : (v01 > 1.f ? 1.f : v01);
    float scale  = env_speed01_ >= 0.5f ? 1.f + (env_speed01_ - 0.5f) * 2.f : env_speed01_ * 2.f;

    for(int op = 0; op < 6; op++)
    {
        int off = op * 21;
        for(int r = 0; r < 4; r++)
        {
            int base_rate = patch_baseline_[off + r];
            int new_rate  = (int)((float)base_rate * scale + 0.5f);
            new_rate      = new_rate < 0 ? 0 : (new_rate > 99 ? 99 : new_rate);
            patch_[off + r] = (uint8_t)new_rate;
        }
    }
    ApplyPatchToHeldVoices();
}

void DexedSynth::SetPatchByte(int byte_index, uint8_t value)
{
    if(byte_index < 0 || byte_index >= 156)
        return;
    value                       = SafePatchByte(byte_index, value);
    patch_[byte_index]          = value;
    patch_baseline_[byte_index] = value;
    if(byte_index >= 137 && byte_index <= 142) // Lfo::reset() reads params[6] from 137
        lfo_.reset(&patch_[137]);
    ApplyPatchToHeldVoices();
}

uint8_t DexedSynth::SafePatchByte(int byte_index, uint8_t value)
{
    // Bytes the engine uses as a table index or a shift count, held to
    // the DX7's own range: a single-voice .syx is copied in raw and a
    // bank passes the pitch EG through unmasked, and a junk byte there
    // read past the algorithm table (writing a whole quantum over the
    // engine's own memory), shifted by a negative feedback or LFO delay
    // amount, read past the pitch EG tables, or overflowed the LFO depth
    // maths (PMD/AMD past 99; AMD's pulled an operator's level below
    // zero). LFO speed past 99 is only a faster LFO, so it stays as is --
    // a dozen factory voices store 127 there. Every other byte is already safe at
    // any value and is left exactly as stored.
    if(byte_index == 134)
        return value & 31; // algorithm, as the bank unpack masks it
    if(byte_index == 135)
        return value & 7; // feedback, likewise
    if((byte_index >= 126 && byte_index <= 133) || (byte_index >= 138 && byte_index <= 140))
        return value > 99 ? 99 : value; // pitch EG rates and levels, LFO delay/PMD/AMD
    return value;
}

int DexedSynth::TransposedNote(int note) const
{
    int t = patch_[144] > 48 ? 24 : patch_[144];
    int n = note + t - 24;
    return n < 0 ? 0 : (n > 127 ? 127 : n);
}

DexedSynth::DexedPresetData DexedSynth::GetInitVoice()
{
    DexedPresetData p;
    for(int op = 0; op < 6; op++)
    {
        uint8_t* o = &p.patch[op * 21];
        for(int i = 0; i < 4; i++)
        {
            o[i]     = 99;               // RATE 1-4
            o[4 + i] = i < 3 ? 99 : 0;   // LEVEL 1-3 = 99, LEVEL 4 = 0
        }
        o[8]  = 39;                      // BREAK POINT = C3
        o[11] = 0;                       // L CURVE = -LIN
        o[12] = 0;                       // R CURVE = -LIN
        o[16] = op == 5 ? 99 : 0;        // OUTPUT LEVEL -- OP1 (index 5) only
        o[17] = 0;                       // OSC MODE = RATIO
        o[18] = 1;                       // F COARSE = 1
        o[20] = 7;                       // OSC DETUNE = 0
    }
    for(int i = 0; i < 4; i++)
    {
        p.patch[126 + i] = 99; // PITCH EG RATE 1-4
        p.patch[130 + i] = 50; // PITCH EG LEVEL 1-4 (no pitch change)
    }
    p.patch[136] = 1;  // OSC KEY SYNC = ON
    p.patch[137] = 35; // LFO SPEED
    p.patch[141] = 1;  // LFO KEY SYNC = ON
    p.patch[143] = 3;  // P MOD SENS
    p.patch[144] = 24; // TRANSPOSE = C3
    std::memcpy(&p.patch[145], "INIT VOICE", 10);
    return p;
}

void DexedSynth::ApplyModWheelTarget()
{
    uint8_t bits = mod_wheel_target_ == ModWheelTarget::Pitch    ? 1  // bit0 = pitch
                   : mod_wheel_target_ == ModWheelTarget::Amp    ? 2  // bit1 = amp
                                                                  : 4; // bit2 = eg
    ctrls_.wheel.setTarget(bits);
}

DexedSynth::DexedPresetData DexedSynth::CapturePreset() const
{
    DexedPresetData p;
    std::memcpy(p.patch, patch_, 156);
    p.reverb_send01   = GetReverbSend01();
    p.output_level01  = GetOutputLevel01();
    p.filter_mode     = (int32_t)GetFilterMode();
    p.filter_cutoff01 = GetFilterCutoff01();
    p.filter_res01    = GetFilterResonance01();
    p.pan01           = GetPan01();
    p.delay_send01    = GetDelaySend01();
    p.fold_amount01   = GetFoldAmount01();
    p.fold_trim01     = GetFoldTrim01();
    p.gain01          = GetGain01();
    p.mod_wheel_target = (int32_t)mod_wheel_target_;
    p.op_switch        = ctrls_.opSwitch;
    p.max_voices       = max_voices_;
    return p;
}

namespace
{
// Resolves a flat 0-based factory preset index to its category and the
// voice index within it -- mirrors the inverse of the removed
// FmSynth::GetFactoryCategorySlot()'s own "walk category counts" idiom.
bool ResolveFactoryIndex(int flat_index, const DexedFactoryCategory** out_cat, int* out_local)
{
    if(flat_index < 0)
        return false;
    for(int c = 0; c < kDexedNumFactoryCategories; c++)
    {
        if(flat_index < kDexedFactoryCategories[c].count)
        {
            *out_cat   = &kDexedFactoryCategories[c];
            *out_local = flat_index;
            return true;
        }
        flat_index -= kDexedFactoryCategories[c].count;
    }
    return false;
}
} // namespace

int DexedSynth::GetNumFactoryPresets()
{
    int total = 0;
    for(int c = 0; c < kDexedNumFactoryCategories; c++)
        total += kDexedFactoryCategories[c].count;
    return total;
}

const char* DexedSynth::GetFactoryCategoryName(int cat)
{
    if(cat < 0 || cat >= kDexedNumFactoryCategories)
        return "?";
    return kDexedFactoryCategories[cat].name;
}

int DexedSynth::GetFactoryCategoryCount(int cat)
{
    if(cat < 0 || cat >= kDexedNumFactoryCategories)
        return 0;
    return kDexedFactoryCategories[cat].count;
}

int DexedSynth::GetFactoryCategorySlot(int cat, int local_index)
{
    if(cat < 0 || cat >= kDexedNumFactoryCategories)
        return -1;
    if(local_index < 0 || local_index >= kDexedFactoryCategories[cat].count)
        return -1;
    int start = 0;
    for(int c = 0; c < cat; c++)
        start += kDexedFactoryCategories[c].count;
    return start + local_index + 1; // slots are 1-based
}

DexedSynth::DexedPresetData DexedSynth::GetFactoryPreset(int flat_index)
{
    DexedPresetData              p;
    const DexedFactoryCategory* cat = nullptr;
    int                          local = 0;
    if(ResolveFactoryIndex(flat_index, &cat, &local))
        DexedSysex::UnpackVoice(cat->packed_data + local * 128, p.patch);
    return p;
}

namespace
{
void UnpackTrimmedVoiceName(const uint8_t* packed_voice, char out[11])
{
    uint8_t unpacked[156];
    DexedSysex::UnpackVoice(packed_voice, unpacked);
    memcpy(out, unpacked + 145, 10);
    out[10] = '\0';
    for(int i = 9; i >= 0 && out[i] == ' '; i--)
        out[i] = '\0';
}
} // namespace

const char* DexedSynth::GetPresetDataName(const DexedPresetData& p)
{
    static char name_buf[11];
    memcpy(name_buf, p.patch + 145, 10);
    name_buf[10] = '\0';
    for(int i = 9; i >= 0 && name_buf[i] == ' '; i--)
        name_buf[i] = '\0';
    return name_buf[0] != '\0' ? name_buf : "?";
}

const char* DexedSynth::GetFactoryPresetName(int flat_index)
{
    static char                 name_buf[11];
    const DexedFactoryCategory* cat   = nullptr;
    int                          local = 0;
    if(!ResolveFactoryIndex(flat_index, &cat, &local))
        return "?";
    UnpackTrimmedVoiceName(cat->packed_data + local * 128, name_buf);

    // Real factory banks occasionally have a long run of voices someone
    // saved without ever renaming from a generic default (e.g. a whole
    // stretch of bare "BELL") -- indistinguishable from each other while
    // scrolling the preset browser (confirmed by real hardware testing:
    // "a file named bells appears the same during a large portion of the
    // K1 sweep"). Disambiguate with a running " N" suffix whenever a
    // name repeats anywhere in this category, numbering every occurrence
    // (including the first) so no two presets in the same category ever
    // display identically.
    int total_matches = 0;
    int this_rank      = 0;
    for(int i = 0; i < cat->count; i++)
    {
        char other[11];
        UnpackTrimmedVoiceName(cat->packed_data + i * 128, other);
        if(strcmp(other, name_buf) == 0)
        {
            total_matches++;
            if(i <= local)
                this_rank = total_matches;
        }
    }
    if(total_matches > 1)
    {
        char suffix[6];
        snprintf(suffix, sizeof(suffix), " %d", this_rank);
        size_t name_len   = strlen(name_buf);
        size_t suffix_len = strlen(suffix);
        if(name_len + suffix_len <= 10)
            strcat(name_buf, suffix);
    }
    return name_buf;
}
