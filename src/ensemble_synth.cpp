#include "ensemble_synth.h"
#include "fast_math.h"
#include "pan_law.h"
#include "svf_setup.h"
#include "itcm.h"
#include <cmath>

namespace
{
// Organ: each drawbar's multiple of its 16' pitch, in Hammond order
// (16', 5 1/3', 8', 4', 2 2/3', 2', 1 3/5', 1 1/3', 1'), and a shared
// one-cycle sine table (plus a guard point for the interpolation).
constexpr uint32_t kOrganHarmonic[EnsembleSynth::kNumOrganDrawbars]
    = {1, 3, 2, 4, 6, 8, 10, 12, 16};
constexpr int   kSineTableBits = 10;
constexpr int   kSineTableSize = 1 << kSineTableBits;
// DTCM: read every sample for every sounding drawbar, so it must not
// compete with the effects' buffers for the cache.
float DSY_DTCM_BSS g_sine_table[kSineTableSize + 1];
// The Organ's registration mixed into one 16' cycle (see
// BuildSineWave()): two buffers, one playing (sine_wave_cur_) while the
// other is rebuilt. Global, not per instance: only the Organ uses them.
float DSY_DTCM_BSS g_sine_wave[2][kSineTableSize + 1];
// A partial above this (as a fraction of the sample rate) folds back an
// octave, as a tonewheel organ's top harmonics do, instead of aliasing.
constexpr float kSineFoldLimit = 0.45f;

// The voice envelopes are worked out once every kEnvStep samples (their
// Adsr runs at sample_rate / kEnvStep) and ramped linearly in between --
// inaudible, and most of each voice's per-sample cost gone.
constexpr int kEnvStep = 8;
// The Ensemble's divide-down bank sat well below every other engine on the
// Mixer meter, so it gets +6 dB over kVoiceGain (the Organ's sines don't
// use the bank, so it's unchanged). Its factory sounds' Gains were then
// matched to peak near -3 dB.
constexpr float kStringsBoost = 2.f;
// New's Gain on the Ensemble: +3 dB, set by ear on the Pod against the
// Organ (the factory sounds are matched to it, see ensemble_factory_data.cpp).
constexpr float kEnsembleNewGain = 3.f / 12.04f;
// Key click length: ~3 ms at 48 kHz.
constexpr int kClickSamples = 144;
// Every voice renders a whole block into this, one voice at a time,
// before the shared chorus/filter/limiter run per sample.
constexpr size_t   kMaxBlock = 256;
float DSY_DTCM_BSS g_ensemble_mix[kMaxBlock];

// One cycle of `table` at phase / 2^32: top bits index it, the next
// ones interpolate.
inline float TableAtPhase(const float* table, uint32_t phase)
{
    constexpr int   kFracBits = 32 - kSineTableBits;
    const uint32_t  i         = phase >> kFracBits;
    const float     frac
        = (float)(phase & ((1u << kFracBits) - 1)) * (1.f / (float)(1u << kFracBits));
    return table[i] + (table[i + 1] - table[i]) * frac;
}

inline float SineOfPhase(uint32_t phase)
{
    return TableAtPhase(g_sine_table, phase);
}

// Same values/reasoning as DexedSynth::IsLimiting()'s own constants.
constexpr float kPreLimiterClipThreshold = 1.f;
constexpr float kClipHoldSeconds         = 0.5f;
constexpr float kMeterReleaseSeconds     = 0.3f;
} // namespace

void EnsembleSynth::Init(float sample_rate)
{
    sample_rate_ = sample_rate;
    for(int i = 0; i <= kSineTableSize; i++)
        g_sine_table[i] = sinf(6.2831853f * (float)i / (float)kSineTableSize);
    for(int i = 0; i < kMaxDrawbars; i++)
        sine_amp_[i] = drawbar_levels_[i] * kVoiceGain;
    SetPercDecay01(perc_decay01_);
    if(organ_)
    {
        BuildSineWave(g_sine_wave[0]);
        sine_wave_cur_   = 0;
        sine_wave_dirty_ = false;
    }
    for(int i = 0; i < kMaxVoices; i++)
    {
        voices_[i].osc.Init(sample_rate_);
        voices_[i].osc.SetGain(kVoiceGain * kStringsBoost);
        SetVoiceRegistration(voices_[i]);
        voices_[i].adsr.Init(sample_rate_, kEnvStep);
    }
    lfo_.Init(sample_rate_, kEnvStep);
    SetAttack01(attack01_);
    SetDecay01(decay01_);
    SetSustain01(sustain01_);
    SetRelease01(release01_);

    chorus_.Init(sample_rate_);
    SetChorusDepth01(chorus_depth01_);
    chorus_depth_smoothed_ = chorus_depth01_;
    chorus_.SetLfoDepth(chorus_depth_smoothed_);
    SetChorusRate01(chorus_rate01_);

    filter_.Init(sample_rate_);
    filter_r_.Init(sample_rate_);

    SetOutputLevel01(output_level01_);
    SetPan01(pan01_);
    if(!organ_)
        SetGain01(kEnsembleNewGain); // power-on matches New
}

// Same 3-tier shape as DexedSynth::FindVoiceForNote(): reuse a voice
// already holding this exact note, else prefer a free voice that's
// confirmed idle (here, via the wrapped Adsr's own IsRunning() --
// OscillatorBank itself has no envelope, so "idle" has to come from
// whatever we wrap around it, same substitution
// SampleSynth::FindVoiceForNote() already makes for the same reason),
// else any free voice, else steal the oldest-triggered one outright.
int EnsembleSynth::FindVoiceForNote(uint8_t note)
{
    for(int i = 0; i < max_voices_; i++)
        if(voices_[i].held_note == (int)note)
            return i;

    int  best_free    = -1;
    bool best_is_idle = false;
    for(int i = 0; i < max_voices_; i++)
    {
        if(voices_[i].held_note != -1)
            continue;
        bool idle = !voices_[i].adsr.IsRunning();
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

void EnsembleSynth::NoteOn(uint8_t note, uint8_t velocity)
{
    int    vi = FindVoiceForNote(note);
    Voice& v  = voices_[vi];

    // Same brief critical section as DexedSynth::NoteOn() -- this runs
    // from ControlTimerCallback (lower NVIC priority than the audio
    // ISR), and SetFreq()+Retrigger() is a real multi-step write across
    // this voice's own internal state, not an atomic flag set. See that
    // function's own comment for the full reasoning (real hardware
    // playtesting found dropped/glitched notes without this, under
    // fast playing).
    __disable_irq();
    SetVoiceFreq(v, 440.f * powf(2.f, ((float)note - 69.f) / 12.f) * pitch_bend_ratio_);
    // Organ: a silent voice starts at phase 0, where every drawbar's
    // sine crosses zero together, so the attack fades in instead of
    // landing mid-cycle (a click, clearest on low notes). A voice still
    // sounding keeps its phase -- resetting that would click instead.
    if(!v.adsr.IsRunning())
    {
        v.sine_phase = 0;
        v.lfo_fade   = 0.f; // the LFO fades in from each new note
    }
    if(!organ_)
    {
        // Velocity on the attack and the tone (see SetVelSens01()).
        const float vn  = (float)velocity * (1.f / 127.f);
        const float att = FastExp2(vel_sens01_ * (80.f / 127.f - vn) * 2.5f);
        v.adsr.SetAttackTime(GetAttackSeconds() * att);
        v.bright = 1.f - vel_sens01_ * (1.f - vn);
        SetVoiceRegistration(v);
        // The note's low-pass: 20 kHz (open) for the hardest note, down
        // to 6 octaves lower for the softest at full Velocity.
        const float octs = vel_sens01_ * (1.f - vn) * 6.f;
        if(octs < 0.05f)
            v.vel_lp_a = 1.f;
        else
        {
            const float fc = 20000.f * FastExp2(-octs);
            v.vel_lp_a     = 1.f - FastExp2(-6.2831853f * 1.4426950f * fc / sample_rate_);
        }
    }
    if(organ_)
    {
        // Single-trigger percussion: only when no other key is held.
        bool others_held = false;
        for(int i = 0; i < kMaxVoices && !others_held; i++)
            others_held = i != vi && voices_[i].held_note >= 0;
        if(perc_on_ && !others_held)
            perc_env_ = 1.f; // restart the shared envelope
        v.click_left = click01_ > 0.f ? kClickSamples : 0;
    }
    v.adsr.Retrigger(false);
    v.held_note    = note;
    v.pitch_note   = note;
    v.velocity     = velocity;
    v.triggered_at = ++trigger_seq_;
    __enable_irq();
}

void EnsembleSynth::SetPitchBend14bit(uint16_t raw14)
{
    // Same +-2 semitone range DexedSynth::SetPitchBend14bit() uses, for
    // consistency across engines.
    float bend01      = ((float)raw14 - 8192.f) / 8192.f; // -1..1
    pitch_bend_ratio_ = powf(2.f, (bend01 * 2.f) / 12.f);
    // Every voice still sounding bends, release tails included, as on
    // any synth -- not only the held keys.
    for(int i = 0; i < kMaxVoices; i++)
    {
        Voice& v = voices_[i];
        if(v.held_note < 0 && !v.adsr.IsRunning())
            continue;
        float note_hz = 440.f * powf(2.f, ((float)v.pitch_note - 69.f) / 12.f);
        // Same IRQ guard as NoteOn(): the Organ's sine_mult[] and
        // sine_folded change together, and the audio ISR must not read
        // them half-written.
        __disable_irq();
        SetVoiceFreq(v, note_hz * pitch_bend_ratio_);
        __enable_irq();
    }
}

void EnsembleSynth::SetVoiceRegistration(Voice& v)
{
    // 0 Saw8', 1 Sq8' as set; 2/3 the 4's x bright; 4/5 the 2's x bright^2;
    // 6 the 1' x bright^3 -- the higher the footage, the more a soft note
    // takes it down.
    const float b = v.bright, b2 = b * b;
    float       amps[kNumDrawbars];
    amps[0] = drawbar_levels_[0];
    amps[1] = drawbar_levels_[1];
    amps[2] = drawbar_levels_[2] * b;
    amps[3] = drawbar_levels_[3] * b;
    amps[4] = drawbar_levels_[4] * b2;
    amps[5] = drawbar_levels_[5] * b2;
    amps[6] = drawbar_levels_[6] * b2 * b;
    v.osc.SetAmplitudes(amps);
}

void EnsembleSynth::SetVoiceFreq(Voice& v, float hz)
{
    v.osc.SetFreq(hz);
    if(!organ_)
        return;
    // The phase runs at half the 16' pitch (a quarter of the note) so a
    // harmonic folded back twice (x5 -> x2.5 -> x1.25) is still a
    // whole-number multiple of it.
    const float inc = hz / sample_rate_;
    v.sine_inc      = (uint32_t)(inc * 0.25f * 4294967296.f);
    bool folded     = false;
    for(int k = 0; k < kNumOrganDrawbars; k++)
    {
        uint32_t m = kOrganHarmonic[k] * 2;
        while(m > 2 && (m & 1) == 0 && (float)m * 0.25f * inc > kSineFoldLimit)
            m >>= 1;
        // Still too high (only far above the keyboard): silent.
        v.sine_mult[k] = (float)m * 0.25f * inc > kSineFoldLimit && m > 2 ? 0 : m;
        folded         = folded || v.sine_mult[k] != kOrganHarmonic[k] * 2;
    }
    v.sine_folded = folded;
}

void EnsembleSynth::BuildSineWave(float* dst)
{
    for(int i = 0; i <= kSineTableSize; i++)
    {
        float s = 0.f;
        for(int k = 0; k < kNumOrganDrawbars; k++)
            if(sine_amp_[k] > 0.f)
                s += g_sine_table[(i * (int)kOrganHarmonic[k]) & (kSineTableSize - 1)] * sine_amp_[k];
        dst[i] = s;
    }
}

void EnsembleSynth::SetModWheel01(float v01)
{
    mod_wheel01_ = v01 < 0.f ? 0.f : (v01 > 1.f ? 1.f : v01);
}

void EnsembleSynth::NoteOff(uint8_t note)
{
    for(int i = 0; i < kMaxVoices; i++)
    {
        if(voices_[i].held_note == (int)note)
        {
            // Freed immediately (see NoteOn()'s own free-voice-
            // preference logic) -- the voice keeps rendering its own
            // real release tail regardless, same as every other engine
            // here. ForceRelease() (not just clearing held_note) is the
            // real fix for a confirmed-on-hardware stuck note: a fast
            // NoteOn immediately followed by this NoteOff, with no
            // audio block ever processed with gate=true in between,
            // left the Adsr's own gate-edge detection never seeing the
            // release -- see Adsr::ForceRelease()'s own doc comment for
            // the full mechanism.
            //
            // held_note goes first: if the audio ISR runs between the
            // two, it sees the gate already low (a normal release). The
            // other order let it see a fresh gate edge on a released
            // envelope and swell back up for a step.
            voices_[i].held_note = -1;
            voices_[i].adsr.ForceRelease();
        }
    }
}

void EnsembleSynth::SetMaxVoices(int n)
{
    max_voices_ = n < 1 ? 1 : (n > kMaxVoices ? kMaxVoices : n);
    // Notes held above the new limit are released (their tails finish).
    for(int i = max_voices_; i < kMaxVoices; i++)
    {
        if(voices_[i].held_note < 0)
            continue;
        voices_[i].held_note = -1; // first -- see NoteOff()
        voices_[i].adsr.ForceRelease();
    }
}

void EnsembleSynth::AllNotesOff()
{
    for(int i = 0; i < kMaxVoices; i++)
    {
        voices_[i].held_note = -1; // first -- see NoteOff()
        voices_[i].adsr.ForceRelease();
    }
}

// The signature control -- 7 real drawbars, directly editable
// (EnsembleParamPage::Registration: K1 selects idx 0-6, K2 sets its
// level). Cached per-voice (drawbar_levels_[]) rather than recomputed
// inside Process()'s own per-sample loop, even though OscillatorBank::
// SetAmplitudes() already no-ops internally when nothing's changed --
// no reason to even make the call 8 times over on a knob that only
// moves between UI redraws, not every sample.
void EnsembleSynth::SetDrawbarLevel(int idx, float level01)
{
    if(idx < 0 || idx >= NumDrawbars())
        return;
    drawbar_levels_[idx] = level01 < 0.f ? 0.f : (level01 > 1.f ? 1.f : level01);
    sine_amp_[idx]       = drawbar_levels_[idx] * kVoiceGain;
    sine_wave_dirty_     = true; // Process() rebuilds and crossfades
    for(int i = 0; i < kMaxVoices; i++)
        SetVoiceRegistration(voices_[i]);
}

float EnsembleSynth::GetDrawbarLevel(int idx) const
{
    if(idx < 0 || idx >= NumDrawbars())
        return 0.f;
    return drawbar_levels_[idx];
}

void EnsembleSynth::SetPercDecay01(float v01)
{
    perc_decay01_ = v01 < 0.f ? 0.f : (v01 > 1.f ? 1.f : v01);
    // Per envelope step: falls to 1/1000 (-60 dB) over the decay time.
    const float steps = GetPercDecaySeconds() * sample_rate_ / (float)kEnvStep;
    perc_coef_        = expf(-6.9078f / steps);
}

float EnsembleSynth::GetPercDecaySeconds() const
{
    return 0.15f * powf(10.f, perc_decay01_); // 0.15 s .. 1.5 s
}

void EnsembleSynth::SetOrgan(bool on)
{
    organ_ = on;
    if(!organ_)
        return;
    // Power-on registration: every drawbar at 60%, the same flat start
    // the Ensemble has -- see GetNewPreset().
    for(int i = 0; i < kNumOrganDrawbars; i++)
        drawbar_levels_[i] = kOrganNewDrawbar;
    attack01_       = 0.05f;
    sustain01_      = 1.f;
    release01_      = 0.25f;
    chorus_depth01_ = 0.2f; // a touch of movement, not the strings' wash
    reverb_send01_  = 0.2f; // a little room
    SetGain01(0.5f);
}

void EnsembleSynth::SetChorusDepth01(float v01)
{
    chorus_depth01_ = v01 < 0.f ? 0.f : (v01 > 1.f ? 1.f : v01);
    // Not set on the chorus here: Process() glides to it (a step clicks).
}

void EnsembleSynth::SetChorusRate01(float v01)
{
    chorus_rate01_ = v01 < 0.f ? 0.f : (v01 > 1.f ? 1.f : v01);
    // 0.05-2 Hz -- a slow, lush ensemble sweep at one end, a faster
    // vibrato-like wobble at the other. Untuned range, retune by ear.
    chorus_.SetLfoFreq(0.05f + chorus_rate01_ * 1.95f);
}

void EnsembleSynth::SetAttack01(float v01)
{
    attack01_ = v01 < 0.f ? 0.f : (v01 > 1.f ? 1.f : v01);
    // Same kMinAdsrSeconds/kMaxAdsrSeconds-style exponential curve
    // SampleSynth's own SetAttack01() uses -- 2ms to 4s, inlined here
    // rather than pulled in as a shared constant since this is the
    // only place in this engine that needs it.
    float seconds = 0.002f * powf(4.f / 0.002f, attack01_);
    for(int i = 0; i < kMaxVoices; i++)
        voices_[i].adsr.SetAttackTime(seconds);
}

void EnsembleSynth::SetDecay01(float v01)
{
    decay01_ = v01 < 0.f ? 0.f : (v01 > 1.f ? 1.f : v01);
    float seconds = 0.002f * powf(4.f / 0.002f, decay01_);
    for(int i = 0; i < kMaxVoices; i++)
        voices_[i].adsr.SetDecayTime(seconds);
}

void EnsembleSynth::SetSustain01(float v01)
{
    sustain01_ = v01 < 0.f ? 0.f : (v01 > 1.f ? 1.f : v01);
    for(int i = 0; i < kMaxVoices; i++)
        voices_[i].adsr.SetSustainLevel(sustain01_);
}

void EnsembleSynth::SetRelease01(float v01)
{
    release01_ = v01 < 0.f ? 0.f : (v01 > 1.f ? 1.f : v01);
    float seconds = 0.002f * powf(4.f / 0.002f, release01_);
    for(int i = 0; i < kMaxVoices; i++)
        voices_[i].adsr.SetReleaseTime(seconds);
}

float EnsembleSynth::GetAttackSeconds() const
{
    return 0.002f * powf(4.f / 0.002f, attack01_);
}

float EnsembleSynth::GetDecaySeconds() const
{
    return 0.002f * powf(4.f / 0.002f, decay01_);
}

float EnsembleSynth::GetReleaseSeconds() const
{
    return 0.002f * powf(4.f / 0.002f, release01_);
}

void EnsembleSynth::SetOutputLevel01(float v01)
{
    output_level01_ = v01 < 0.f ? 0.f : (v01 > 1.f ? 1.f : v01);
    // Same curve every other engine's own Output Level uses.
    output_level_ = powf(output_level01_, 2.5f) * 1.4f;
}

void EnsembleSynth::SetPan01(float v01)
{
    pan01_      = v01 < 0.f ? 0.f : (v01 > 1.f ? 1.f : v01);
    EqualPowerPan(pan01_, &pan_l_gain_, &pan_r_gain_);
}

EnsembleSynth::EnsemblePresetData EnsembleSynth::CapturePreset() const
{
    EnsemblePresetData p;
    const int lo = organ_ ? 2 : 0; // see ApplyPreset()
    for(int i = 0; i < kNumDrawbars; i++)
        p.drawbar_levels[i] = drawbar_levels_[lo + i];
    if(organ_)
    {
        p.organ_low_drawbars[0] = drawbar_levels_[0];
        p.organ_low_drawbars[1] = drawbar_levels_[1];
    }
    p.chorus_depth01       = chorus_depth01_;
    p.chorus_rate01        = chorus_rate01_;
    p.attack01             = attack01_;
    p.decay01              = decay01_;
    p.sustain01            = sustain01_;
    p.release01            = release01_;
    p.filter_mode          = (int32_t)filter_mode_;
    p.filter_cutoff01      = filter_cutoff01_;
    p.filter_res01         = filter_res01_;
    p.output_level01       = output_level01_;
    p.pan01                = pan01_;
    p.reverb_send01        = reverb_send01_;
    p.delay_send01         = delay_send01_;
    p.gain01               = gain01_;
    p.mod_wheel_target     = (int32_t)mod_wheel_target_;
    p.max_voices           = max_voices_;
    p.perc_on              = perc_on_ ? 1 : 0;
    p.perc_third           = perc_third_ ? 1 : 0;
    p.perc_decay01         = perc_decay01_;
    p.perc_level01         = perc_level01_;
    p.click01              = click01_;
    p.vel_sens01           = vel_sens01_;
    p.lfo_wave             = (int32_t)lfo_.GetWave();
    p.lfo_rate01           = lfo_.GetRate01();
    p.lfo_sync             = lfo_.GetSync() ? 1 : 0;
    p.lfo_sync_index       = lfo_.GetSyncIndex();
    p.lfo_fade01           = lfo_.GetFade01();
    for(int d = 0; d < SynthLfo::kNumDests; d++)
        p.lfo_depth01[d] = lfo_.GetDepth01(d);
    return p;
}

EnsembleSynth::EnsemblePresetData EnsembleSynth::GetNewPreset() const
{
    EnsemblePresetData p;
    if(!organ_)
    {
        p.gain01 = kEnsembleNewGain;
        return p;
    }
    // The Organ's New matches its power-on sound (SetOrgan()).
    for(int i = 0; i < kNumDrawbars; i++)
        p.drawbar_levels[i] = kOrganNewDrawbar;
    p.organ_low_drawbars[0] = kOrganNewDrawbar;
    p.organ_low_drawbars[1] = kOrganNewDrawbar;
    p.attack01              = 0.05f;
    p.sustain01             = 1.f;
    p.release01             = 0.25f;
    p.chorus_depth01        = 0.2f;
    p.gain01                = 0.5f;
    p.reverb_send01         = 0.2f;
    return p;
}

int EnsembleSynth::GetNumFactoryPresets() const
{
    return organ_ ? kOrganNumFactoryPresets : kEnsembleNumFactoryPresets;
}

const char* EnsembleSynth::GetFactoryPresetName(int flat_index) const
{
    if(flat_index < 0 || flat_index >= GetNumFactoryPresets())
        return "?";
    return organ_ ? kOrganFactoryPresets[flat_index].name : kEnsembleFactoryPresets[flat_index].name;
}

// Both banks share every field but the drawbars.
template <typename F>
static void FillFactoryCommon(EnsembleSynth::EnsemblePresetData& p, const F& f)
{
    p.chorus_depth01                = f.chorus_depth01;
    p.chorus_rate01                 = f.chorus_rate01;
    p.attack01                      = f.attack01;
    p.decay01                       = f.decay01;
    p.sustain01                     = f.sustain01;
    p.release01                     = f.release01;
    p.filter_mode                   = f.filter_mode;
    p.filter_cutoff01                = f.filter_cutoff01;
    p.filter_res01                   = f.filter_res01;
    p.output_level01                 = f.output_level01;
    p.pan01                          = f.pan01;
    p.reverb_send01                  = f.reverb_send01;
    p.delay_send01                   = f.delay_send01;
    p.gain01                         = f.gain01;
}

EnsembleSynth::EnsemblePresetData EnsembleSynth::GetFactoryPreset(int flat_index) const
{
    EnsemblePresetData p;
    if(flat_index < 0 || flat_index >= GetNumFactoryPresets())
        return p;
    if(organ_)
    {
        // Hammond order -> the preset's 8'..1' plus its 16'/5 1/3' pair.
        const OrganFactoryPreset& f = kOrganFactoryPresets[flat_index];
        for(int i = 0; i < kNumDrawbars; i++)
            p.drawbar_levels[i] = f.drawbar_levels[2 + i];
        p.organ_low_drawbars[0] = f.drawbar_levels[0];
        p.organ_low_drawbars[1] = f.drawbar_levels[1];
        FillFactoryCommon(p, f);
    }
    else
    {
        const EnsembleFactoryPreset& f = kEnsembleFactoryPresets[flat_index];
        for(int i = 0; i < kNumDrawbars; i++)
            p.drawbar_levels[i] = f.drawbar_levels[i];
        FillFactoryCommon(p, f);
        const EnsembleFactoryExtra& x = kEnsembleFactoryExtras[flat_index];
        p.vel_sens01                  = x.vel_sens01;
        p.lfo_wave                    = x.lfo_wave;
        p.lfo_rate01                  = x.lfo_rate01;
        p.lfo_sync                    = x.lfo_sync;
        p.lfo_sync_index              = x.lfo_sync_index;
        p.lfo_fade01                  = x.lfo_fade01;
        p.lfo_depth01[SynthLfo::kPitch]  = x.lfo_pitch01;
        p.lfo_depth01[SynthLfo::kFilter] = x.lfo_filter01;
        p.lfo_depth01[SynthLfo::kAmp]    = x.lfo_amp01;
    }
    return p;
}

DSY_ITCM_TEXT
void EnsembleSynth::Process(size_t size, float* out_l, float* out_r, float* reverb_send_l,
                             float* reverb_send_r, float* delay_send_l, float* delay_send_r)
{
    // Per-block constants (were divisions every sample).
    const float meter_release = 1.f / (kMeterReleaseSeconds * sample_rate_);
    const float inv_sr        = 1.f / sample_rate_;
    if(size > kMaxBlock)
        size = kMaxBlock;

    // The LFO, one value per envelope step -- run whether or not anything
    // sounds, so a synced one stays on the beat. Not on the Organ.
    float     lfo_at[kMaxBlock / kEnvStep + 2] = {};
    const int lfo_steps                         = (int)((size + kEnvStep - 1) / kEnvStep);
    if(!organ_)
        lfo_.Render(lfo_steps, lfo_at);
    const float lfo_amp   = organ_ ? 0.f : lfo_.Depth(SynthLfo::kAmp);
    const float lfo_pitch = organ_ ? 0.f : lfo_.Depth(SynthLfo::kPitch);
    const float lfo_filt  = organ_ ? 0.f : lfo_.Depth(SynthLfo::kFilter);

    // Silent and nothing pending: skip all the per-sample work (the same
    // rule as GranularEngine::Process()). Output is plain zero, the sends
    // get nothing, and the meters decay as they would have.
    {
        bool any_voice = false;
        for(int vi = 0; vi < kMaxVoices && !any_voice; vi++)
            any_voice = voices_[vi].held_note >= 0 || voices_[vi].adsr.IsRunning();
        bool silent = !any_voice;
        if(silent && last_block_peak_ < 1e-6f)
        {
            for(size_t i = 0; i < size; i++)
                out_l[i] = out_r[i] = 0.f;
            pre_limiter_level_ -= pre_limiter_level_ * meter_release * (float)size;
            if(pre_limiter_level_ < 0.f)
                pre_limiter_level_ = 0.f;
            if(pre_limiter_clip_hold_ > 0.f)
                pre_limiter_clip_hold_ -= inv_sr * (float)size;
            return;
        }
    }
    float block_peak = 0.f;
    // Mod wheel -- off by default (ModWheelTarget::Off), explicitly
    // picked via CycleModWheelTarget(). Pushes whichever target is
    // selected UP from its own knob-set base value toward 1.0, scaled
    // by wheel position -- never below the base, and wheel=0 always
    // reads as "unchanged" -- deliberately NOT an independent sweep or
    // a forced mode-on: FilterCutoff below only has anything to
    // modulate if Filter Mode is already something other than Off,
    // same "behaves exactly as the parameters are already set" choice
    // made explicitly over forcing LowPass on.
    float eff_chorus_depth01  = chorus_depth01_;
    float eff_filter_cutoff01 = filter_cutoff01_;
    float eff_output_level_   = output_level_;
    switch(mod_wheel_target_)
    {
        case ModWheelTarget::ChorusDepth:
            eff_chorus_depth01 = chorus_depth01_ + mod_wheel01_ * (1.f - chorus_depth01_);
            break;
        case ModWheelTarget::FilterCutoff:
            eff_filter_cutoff01 = filter_cutoff01_ + mod_wheel01_ * (1.f - filter_cutoff01_);
            break;
        case ModWheelTarget::OutputLevel:
        {
            float eff_output_level01 = output_level01_ + mod_wheel01_ * (1.f - output_level01_);
            eff_output_level_        = powf(eff_output_level01, 2.5f) * 1.4f;
            break;
        }
        default: break;
    }
    // Chorus depth glides to its target (knob and mod wheel) over ~20 ms
    // instead of stepping once a block: the chorus delay is LFO x depth,
    // so a step jumps the read point -- a click, clear on the Organ's
    // sines, every time the wheel moved one of its 128 steps.
    const float depth_target = eff_chorus_depth01;
    const float depth_glide  = 1.f / (0.02f * sample_rate_);

    // Block-rate filter cutoff/res -- same curve/guard every other
    // engine's own bus filter uses.
    if(filter_mode_ != FilterMode::Off)
    {
        float cutoff_hz = kFilterMinHz * powf(kFilterMaxHz / kFilterMinHz, eff_filter_cutoff01);
        // The LFO's Filter depth: up to 4 octaves either way.
        if(lfo_filt != 0.f)
            cutoff_hz *= FastExp2(lfo_at[0] * lfo_filt * 4.f);
        float nyquist_guard = sample_rate_ / 3.f - 1.f;
        cutoff_hz = cutoff_hz < kFilterMinHz ? kFilterMinHz
                    : cutoff_hz > nyquist_guard ? nyquist_guard
                                                : cutoff_hz;
        filter_.SetFreq(cutoff_hz);
        filter_.SetRes(filter_res01_ * 0.9f);
        filter_r_.SetFreq(cutoff_hz);
        filter_r_.SetRes(filter_res01_ * 0.9f);
    }

    const float send       = reverb_send01_;
    const float delay_send = delay_send01_;
    const bool  sine       = organ_;
    // A drawbar moved: rebuild the mixed wave into the idle buffer and
    // crossfade old -> new across this block (see BuildSineWave()).
    const float* sine_prev = nullptr;
    if(sine && sine_wave_dirty_)
    {
        sine_wave_dirty_ = false;
        const int next   = 1 - sine_wave_cur_;
        BuildSineWave(g_sine_wave[next]);
        sine_prev      = g_sine_wave[sine_wave_cur_];
        sine_wave_cur_ = next;
    }
    const float* sine_wave  = g_sine_wave[sine_wave_cur_];
    const float  xfade_step = 1.f / (float)size;
    // Percussion: 4' or 2 2/3' (drawbar 3 or 4's multiple, already folded
    // back for high notes). At Volume 100% it peaks at three full
    // drawbars, so it cuts through a full registration as a Hammond's
    // Normal percussion does. The shared envelope's value at each
    // envelope step of this block, for every voice to ramp between.
    if(size > kMaxBlock) // before perc_at[] below, which is sized for kMaxBlock
        size = kMaxBlock;
    const int   perc_bar  = perc_third_ ? 4 : 3;
    const float perc_gain = perc_on_ ? perc_level01_ * kVoiceGain * 3.f : 0.f;
    float       perc_at[kMaxBlock / kEnvStep + 2];
    const int   n_steps = (int)((size + kEnvStep - 1) / kEnvStep);
    perc_at[0]          = perc_env_;
    for(int k = 0; k < n_steps; k++)
        perc_at[k + 1] = perc_at[k] * perc_coef_;
    perc_env_ = perc_at[n_steps] < 1e-4f ? 0.f : perc_at[n_steps];
    const float click_gain = click01_ * kVoiceGain * 3.f;

    // The LFO's vibrato: each sounding voice's pitch, once a block, with
    // its own fade -- up to 12 semitones (a squared curve, fine near 0).
    // Once more after the depth goes back to 0, to put the pitch back.
    if(lfo_pitch != 0.f || lfo_pitch_was_on_)
    {
        const float semis = lfo_pitch * (lfo_pitch < 0.f ? -lfo_pitch : lfo_pitch) * 12.f;
        for(int vi = 0; vi < kMaxVoices; vi++)
        {
            Voice& v = voices_[vi];
            if(v.held_note < 0 && !v.adsr.IsRunning())
                continue;
            const float hz = 440.f * FastExp2(((float)v.pitch_note - 69.f
                                               + lfo_at[0] * semis * v.lfo_fade)
                                              * (1.f / 12.f));
            SetVoiceFreq(v, hz * pitch_bend_ratio_);
        }
        lfo_pitch_was_on_ = lfo_pitch != 0.f;
    }

    // Voices first, one at a time across the whole block: each sounding
    // voice's state stays in registers, and idle ones are skipped once
    // per block rather than once per sample.
    for(size_t i = 0; i < size; i++)
        g_ensemble_mix[i] = 0.f;
    for(int vi = 0; vi < kMaxVoices; vi++)
    {
        Voice&     v    = voices_[vi];
        const bool gate = v.held_note >= 0;
        if(!gate && !v.adsr.IsRunning())
            continue; // fully idle -- stop paying for this voice at all
        // Velocity scales the envelope (see SetVelSens01()), so it costs
        // nothing per sample.
        // Velocity squared -- the usual DLS/MIDI curve, so at full Velocity
        // 40 is about 19 dB under 120.
        const float vn  = (float)v.velocity * (1.f / 127.f);
        const float vel = 1.f - vel_sens01_ * (1.f - vn * vn);
        for(size_t i0 = 0; i0 < size; i0 += kEnvStep)
        {
            const size_t n      = size - i0 < (size_t)kEnvStep ? size - i0 : (size_t)kEnvStep;
            // env_last is the level actually played (velocity and the
            // LFO's Amp in it), so a change ramps across the step.
            const int   lstep = (int)(i0 / kEnvStep);
            const float fade  = v.lfo_fade;
            v.lfo_fade        = fade + lfo_.FadeStep() > 1.f ? 1.f : fade + lfo_.FadeStep();
            float ampf        = vel;
            if(lfo_amp != 0.f)
            {
                // Full depth swings 1 -> 0 (tremolo, or a gate when the
                // wave's Square); below centre the swing is inverted.
                const float lf = lfo_amp >= 0.f ? lfo_at[lstep] : -lfo_at[lstep];
                const float la = lfo_amp >= 0.f ? lfo_amp : -lfo_amp;
                ampf *= 1.f - la * fade * (1.f - lf) * 0.5f;
            }
            const float  e_from = v.env_last;
            const float  e_to   = v.adsr.Process(gate) * ampf;
            v.env_last          = e_to;
            const float  de     = (e_to - e_from) / (float)n;
            float        env    = e_from;
            float*       mix    = g_ensemble_mix + i0;
            // Organ percussion, ramped across the step like the envelope.
            const int   step   = (int)(i0 / kEnvStep);
            const float p_from = perc_at[step] * perc_gain;
            const float dp     = (perc_at[step + 1] * perc_gain - p_from) / (float)n;
            float       perc  = p_from;
            const bool  do_pc = sine && (p_from > 0.f) && v.sine_mult[perc_bar] != 0;
            if(!sine && v.vel_lp_a >= 1.f)
            {
                for(size_t j = 0; j < n; j++)
                {
                    env += de;
                    mix[j] += v.osc.Process() * env;
                }
            }
            else if(!sine)
            {
                // Velocity's low-pass (see SetVelSens01()).
                const float a  = v.vel_lp_a;
                float       y  = v.vel_lp_y;
                float       y2 = v.vel_lp_y2;
                for(size_t j = 0; j < n; j++)
                {
                    env += de;
                    y += a * (v.osc.Process() - y);
                    y2 += a * (y - y2);
                    mix[j] += y2 * env;
                }
                v.vel_lp_y  = y;
                v.vel_lp_y2 = y2;
            }
            else if(!v.sine_folded)
            {
                // The phase runs at half the 16' pitch (see
                // SetVoiceFreq()), so x2 is one cycle of the mixed wave.
                uint32_t ph = v.sine_phase;
                for(size_t j = 0; j < n; j++)
                {
                    ph += v.sine_inc;
                    env += de;
                    float w = TableAtPhase(sine_wave, ph * 2u);
                    if(sine_prev)
                    {
                        const float x = (float)(i0 + j + 1) * xfade_step;
                        w = TableAtPhase(sine_prev, ph * 2u) * (1.f - x) + w * x;
                    }
                    if(do_pc)
                    {
                        perc += dp;
                        w += SineOfPhase(ph * v.sine_mult[perc_bar]) * perc;
                    }
                    mix[j] += w * env;
                }
                v.sine_phase = ph;
            }
            else
            {
                uint32_t ph = v.sine_phase;
                for(size_t j = 0; j < n; j++)
                {
                    ph += v.sine_inc;
                    env += de;
                    float s = 0.f;
                    for(int k = 0; k < kNumOrganDrawbars; k++)
                        if(sine_amp_[k] > 0.f && v.sine_mult[k] != 0)
                            s += SineOfPhase(ph * v.sine_mult[k]) * sine_amp_[k];
                    if(do_pc)
                    {
                        perc += dp;
                        s += SineOfPhase(ph * v.sine_mult[perc_bar]) * perc;
                    }
                    mix[j] += s * env;
                }
                v.sine_phase = ph;
            }
            // Key click: lowpassed noise, fading out over kClickSamples.
            if(v.click_left > 0)
            {
                for(size_t j = 0; j < n && v.click_left > 0; j++, v.click_left--)
                {
                    v.click_rng      = v.click_rng * 1664525u + 1013904223u;
                    const float nz   = (float)(int32_t)v.click_rng * (1.f / 2147483648.f);
                    v.click_lp      += (nz - v.click_lp) * 0.35f;
                    mix[j] += v.click_lp * click_gain * ((float)v.click_left / (float)kClickSamples);
                }
            }
        }
    }

    for(size_t i = 0; i < size; i++)
    {
        const float mix = g_ensemble_mix[i];

        // Shared Chorus -- ONE instance on the summed mix, not one per
        // voice (see the class's own doc comment for the real measured
        // reason). Stereo (see fast_chorus.h).
        // Set only while it is still gliding: once the glide settles the
        // value stops changing.
        const float depth_was = chorus_depth_smoothed_;
        chorus_depth_smoothed_ += (depth_target - chorus_depth_smoothed_) * depth_glide;
        if(chorus_depth_smoothed_ != depth_was)
            chorus_.SetLfoDepth(chorus_depth_smoothed_);
        float l, r;
        chorus_.Process(mix, l, r);
        const bool stereo = chorus_.GetStereo();

        if(filter_mode_ != FilterMode::Off)
        {
            filter_.Process(l);
            if(stereo)
                filter_r_.Process(r);
            switch(filter_mode_)
            {
                case FilterMode::LowPass: l = filter_.Low(); r = filter_r_.Low(); break;
                case FilterMode::HighPass: l = filter_.High(); r = filter_r_.High(); break;
                case FilterMode::BandPass: l = filter_.Band(); r = filter_r_.Band(); break;
                default: break;
            }
            if(!stereo)
                r = l;
        }

        // See IsLimiting()'s/GetPreLimiterLevel01()'s own comments --
        // both read the PRE-limiter value, max(|l|,|r|) since the chorus
        // is stereo. Same instant-attack/slow-release peak shape
        // main.cpp's own meters use.
        // Gain into the limiter (see SetGain01()) -- before the meter
        // below, so the Mixer's clip indicator shows when it's pushed.
        l *= gain_;
        r *= gain_;
        float pre_mag = fabsf(l) > fabsf(r) ? fabsf(l) : fabsf(r);
        if(pre_mag > pre_limiter_level_)
            pre_limiter_level_ = pre_mag;
        else
            pre_limiter_level_ -= pre_limiter_level_ * meter_release;
        if(pre_mag >= kPreLimiterClipThreshold)
            pre_limiter_clip_hold_ = kClipHoldSeconds;
        else if(pre_limiter_clip_hold_ > 0.f)
            pre_limiter_clip_hold_ -= inv_sr;

        // Soft limiter -- same FastTanh()-as-safety-net convention every
        // other engine here uses, on top of each voice's own fixed
        // kVoiceGain margin (see that constant's own comment) rather
        // than a separate post-sum headroom multiplier.
        l = FastTanh(l) * eff_output_level_;
        r = FastTanh(r) * eff_output_level_;

        // Same equal-power pan law as every other channel (see
        // pan_law.h), applied to this already-stereo pair.
        float sl = l * pan_l_gain_;
        float sr = r * pan_r_gain_;
        float pk = fabsf(sl) > fabsf(sr) ? fabsf(sl) : fabsf(sr);
        if(pk > block_peak)
            block_peak = pk;
        out_l[i] = sl;
        out_r[i] = sr;
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
