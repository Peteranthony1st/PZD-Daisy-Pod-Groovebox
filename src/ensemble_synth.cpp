#include "ensemble_synth.h"
#include "svf_setup.h"
#include "itcm.h"
#include <cmath>

namespace
{
// Same values/reasoning as DexedSynth::IsLimiting()'s own constants.
constexpr float kPreLimiterClipThreshold = 1.f;
constexpr float kClipHoldSeconds         = 0.5f;
constexpr float kMeterReleaseSeconds     = 0.3f;
} // namespace

void EnsembleSynth::Init(float sample_rate)
{
    sample_rate_ = sample_rate;
    for(int i = 0; i < kMaxVoices; i++)
    {
        voices_[i].osc.Init(sample_rate_);
        voices_[i].osc.SetGain(kVoiceGain);
        voices_[i].osc.SetAmplitudes(drawbar_levels_);
        voices_[i].adsr.Init(sample_rate_);
    }
    SetAttack01(attack01_);
    SetDecay01(decay01_);
    SetSustain01(sustain01_);
    SetRelease01(release01_);

    chorus_.Init(sample_rate_);
    SetChorusDepth01(chorus_depth01_);
    SetChorusRate01(chorus_rate01_);

    filter_l_.Init(sample_rate_);
    filter_r_.Init(sample_rate_);

    SetOutputLevel01(output_level01_);
    SetPan01(pan01_);
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
    for(int i = 0; i < kMaxVoices; i++)
        if(voices_[i].held_note == (int)note)
            return i;

    int  best_free    = -1;
    bool best_is_idle = false;
    for(int i = 0; i < kMaxVoices; i++)
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
    for(int i = 1; i < kMaxVoices; i++)
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
    v.osc.SetFreq(440.f * powf(2.f, ((float)note - 69.f) / 12.f) * pitch_bend_ratio_);
    v.adsr.Retrigger(false);
    v.held_note    = note;
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
    for(int i = 0; i < kMaxVoices; i++)
    {
        if(voices_[i].held_note < 0)
            continue;
        float note_hz = 440.f * powf(2.f, ((float)voices_[i].held_note - 69.f) / 12.f);
        voices_[i].osc.SetFreq(note_hz * pitch_bend_ratio_);
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
            voices_[i].adsr.ForceRelease();
            voices_[i].held_note = -1;
        }
    }
}

void EnsembleSynth::AllNotesOff()
{
    for(int i = 0; i < kMaxVoices; i++)
    {
        voices_[i].adsr.ForceRelease();
        voices_[i].held_note = -1;
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
    if(idx < 0 || idx >= kNumDrawbars)
        return;
    drawbar_levels_[idx] = level01 < 0.f ? 0.f : (level01 > 1.f ? 1.f : level01);
    for(int i = 0; i < kMaxVoices; i++)
        voices_[i].osc.SetAmplitudes(drawbar_levels_);
}

float EnsembleSynth::GetDrawbarLevel(int idx) const
{
    if(idx < 0 || idx >= kNumDrawbars)
        return 0.f;
    return drawbar_levels_[idx];
}

void EnsembleSynth::SetChorusDepth01(float v01)
{
    chorus_depth01_ = v01 < 0.f ? 0.f : (v01 > 1.f ? 1.f : v01);
    chorus_.SetLfoDepth(chorus_depth01_);
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
    pan_l_gain_ = 1.f - pan01_;
    pan_r_gain_ = pan01_;
}

EnsembleSynth::EnsemblePresetData EnsembleSynth::CapturePreset() const
{
    EnsemblePresetData p;
    for(int i = 0; i < kNumDrawbars; i++)
        p.drawbar_levels[i] = drawbar_levels_[i];
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
    return p;
}

int EnsembleSynth::GetNumFactoryPresets()
{
    return kEnsembleNumFactoryPresets;
}

const char* EnsembleSynth::GetFactoryPresetName(int flat_index)
{
    if(flat_index < 0 || flat_index >= kEnsembleNumFactoryPresets)
        return "?";
    return kEnsembleFactoryPresets[flat_index].name;
}

EnsembleSynth::EnsemblePresetData EnsembleSynth::GetFactoryPreset(int flat_index)
{
    EnsemblePresetData p;
    if(flat_index < 0 || flat_index >= kEnsembleNumFactoryPresets)
        return p;
    const EnsembleFactoryPreset& f = kEnsembleFactoryPresets[flat_index];
    for(int i = 0; i < kNumDrawbars; i++)
        p.drawbar_levels[i] = f.drawbar_levels[i];
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
    return p;
}

DSY_ITCM_TEXT
void EnsembleSynth::Process(size_t size, float* out_l, float* out_r, float* reverb_send_l,
                             float* reverb_send_r, float* delay_send_l, float* delay_send_r)
{
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
    chorus_.SetLfoDepth(eff_chorus_depth01);

    // Block-rate filter cutoff/res -- same curve/guard every other
    // engine's own bus filter uses.
    if(filter_mode_ != FilterMode::Off)
    {
        float cutoff_hz = kFilterMinHz * powf(kFilterMaxHz / kFilterMinHz, eff_filter_cutoff01);
        float nyquist_guard = sample_rate_ / 3.f - 1.f;
        cutoff_hz = cutoff_hz < kFilterMinHz ? kFilterMinHz
                    : cutoff_hz > nyquist_guard ? nyquist_guard
                                                : cutoff_hz;
        filter_l_.SetFreq(cutoff_hz);
        filter_l_.SetRes(filter_res01_ * 0.9f);
        filter_r_.SetFreq(cutoff_hz);
        filter_r_.SetRes(filter_res01_ * 0.9f);
    }

    const float send       = reverb_send01_;
    const float delay_send = delay_send01_;

    for(size_t i = 0; i < size; i++)
    {
        float mix = 0.f;
        for(int vi = 0; vi < kMaxVoices; vi++)
        {
            Voice& v    = voices_[vi];
            bool   gate = v.held_note >= 0;
            if(!gate && !v.adsr.IsRunning())
                continue; // fully idle -- stop paying for this voice at all
            float env = v.adsr.Process(gate);
            mix += v.osc.Process() * env;
        }

        // Shared Chorus -- ONE instance on the summed mix, not one per
        // voice (see the class's own doc comment for the real measured
        // reason). Genuinely stereo: GetLeft()/GetRight() feed two
        // real, different signals into the filter pair below, not the
        // same mono value processed twice the way Dexed's own
        // filter_l_/filter_r_ do.
        chorus_.Process(mix);
        float l = chorus_.GetLeft();
        float r = chorus_.GetRight();

        if(filter_mode_ != FilterMode::Off)
        {
            filter_l_.Process(l);
            filter_r_.Process(r);
            switch(filter_mode_)
            {
                case FilterMode::LowPass: l = filter_l_.Low(); r = filter_r_.Low(); break;
                case FilterMode::HighPass: l = filter_l_.High(); r = filter_r_.High(); break;
                case FilterMode::BandPass: l = filter_l_.Band(); r = filter_r_.Band(); break;
                default: break;
            }
        }

        // See IsLimiting()'s/GetPreLimiterLevel01()'s own comments --
        // both read the PRE-limiter value, max(|l|,|r|) since this is
        // genuinely stereo (unlike Dexed's own mono-then-panned
        // signal). Same instant-attack/slow-release peak shape
        // main.cpp's own meters use.
        // Gain into the limiter (see SetGain01()) -- before the meter
        // below, so the Mixer's clip indicator shows when it's pushed.
        l *= gain_;
        r *= gain_;
        float pre_mag = fabsf(l) > fabsf(r) ? fabsf(l) : fabsf(r);
        if(pre_mag > pre_limiter_level_)
            pre_limiter_level_ = pre_mag;
        else
            pre_limiter_level_ -= pre_limiter_level_ * (1.f / (kMeterReleaseSeconds * sample_rate_));
        if(pre_mag >= kPreLimiterClipThreshold)
            pre_limiter_clip_hold_ = kClipHoldSeconds;
        else if(pre_limiter_clip_hold_ > 0.f)
            pre_limiter_clip_hold_ -= 1.f / sample_rate_;

        // Soft limiter -- same tanhf()-as-safety-net convention every
        // other engine here uses, on top of each voice's own fixed
        // kVoiceGain margin (see that constant's own comment) rather
        // than a separate post-sum headroom multiplier.
        l = tanhf(l) * eff_output_level_;
        r = tanhf(r) * eff_output_level_;

        // Same linear pan law/gain formula every other engine's own
        // Pan uses (pan_l_gain_/pan_r_gain_ = 1-v/v), applied directly
        // and literally identically to this already-stereo pair --
        // deliberately NOT a different "balance" law that would keep
        // one side at unity, so this knob feels consistent across
        // every instrument in this project. Real consequence worth
        // knowing: center (the default) attenuates both channels to
        // 0.5x, same as it does for every other engine's own mono
        // signal being split -- here that's a real level drop on an
        // already-stereo source, not just a style choice, so this is
        // one more thing the real hardware tuning pass needs to catch,
        // not something to compensate for blindly here.
        float sl = l * pan_l_gain_;
        float sr = r * pan_r_gain_;
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
}
