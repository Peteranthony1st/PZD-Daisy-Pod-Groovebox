#include "sample_synth.h"
#include "itcm.h"
#include <cmath>
#include <cstring>

void SampleSynth::Init(float sample_rate)
{
    sample_rate_ = sample_rate;
    for(int i = 0; i < kMaxVoices; i++)
        voices_[i].adsr.Init(sample_rate_);
    SetAttack01(attack01_);
    SetDecay01(decay01_);
    SetSustain01(sustain01_);
    SetRelease01(release01_);

    filter_l_.Init(sample_rate_);
    filter_r_.Init(sample_rate_);

    SetOutputLevel01(output_level01_);
    SetPan01(pan01_);
}

void SampleSynth::ClearSlot(int index)
{
    if(index < 0 || index >= kMaxSampleSlots)
        return;
    slots_[index] = SampleSlot{};
}

void SampleSynth::LoadSlot(int index, const float* buf_l, const float* buf_r, int32_t len,
                             bool chromatic, uint8_t root_note, uint8_t low_note,
                             uint8_t high_note, float volume01)
{
    if(index < 0 || index >= kMaxSampleSlots)
        return;
    SampleSlot& s = slots_[index];
    s.buf_l      = buf_l;
    s.buf_r      = buf_r;
    s.len        = len;
    s.chromatic  = chromatic;
    s.root_note  = root_note;
    s.low_note   = low_note;
    s.high_note  = high_note;
    s.volume01   = volume01;
}

void SampleSynth::ResetAllVoices()
{
    for(int i = 0; i < kMaxVoices; i++)
    {
        voices_[i] = Voice{};
        voices_[i].adsr.Init(sample_rate_); // Voice{} default-constructs a fresh, un-Init'd Adsr
    }
}

int SampleSynth::FindVoiceForNote(uint8_t note)
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

void SampleSynth::NoteOn(uint8_t note, uint8_t velocity)
{
    int match = -1;
    for(int i = 0; i < kMaxSampleSlots; i++)
    {
        const SampleSlot& s = slots_[i];
        if(s.len > 0 && note >= s.low_note && note <= s.high_note)
        {
            match = i;
            break;
        }
    }
    if(match < 0)
        return; // no slot answers to this note -- a real, correct no-op

    int    vi = FindVoiceForNote(note);
    Voice& v  = voices_[vi];
    const SampleSlot& s = slots_[match];

    // Same brief critical section as DexedSynth::NoteOn()/
    // EnsembleSynth::NoteOn() -- this runs from ControlTimerCallback
    // (lower NVIC priority than the audio ISR), and this is a real
    // multi-field write across this voice's own internal state, not an
    // atomic flag set. See those functions' own comments for the full
    // reasoning (real hardware playtesting found dropped/glitched notes
    // without this, under fast playing).
    __disable_irq();
    v.held_note    = note;
    v.velocity     = velocity;
    v.triggered_at = ++trigger_seq_;
    v.slot         = match;
    v.read_pos     = 0.f;
    // Same 2^((note-root)/12) formula GranularEngine::NoteOn() already
    // uses -- fixed-pitch (drum/one-shot) slots ignore the incoming
    // note's pitch entirely, same reasoning a drum machine pad plays
    // its sample at native pitch regardless of which key triggered it.
    v.read_inc = s.chromatic ? powf(2.f, ((float)note - (float)s.root_note) / 12.f) : 1.f;
    v.adsr.Retrigger(false);
    __enable_irq();
}

void SampleSynth::NoteOff(uint8_t note)
{
    for(int i = 0; i < kMaxVoices; i++)
    {
        if(voices_[i].held_note == (int)note)
        {
            // ForceRelease() (not just clearing held_note) is the real
            // fix for a confirmed-on-hardware stuck note found in
            // EnsembleSynth -- this engine shares the identical risk
            // (same gate-sampled daisysp::Adsr pattern). See
            // Adsr::ForceRelease()'s own doc comment for the full
            // mechanism.
            voices_[i].adsr.ForceRelease();
            voices_[i].held_note = -1; // release tail keeps rendering via adsr
        }
    }
}

void SampleSynth::AllNotesOff()
{
    for(int i = 0; i < kMaxVoices; i++)
    {
        voices_[i].adsr.ForceRelease();
        voices_[i].held_note = -1;
    }
}

void SampleSynth::SetPitchBend14bit(uint16_t raw14)
{
    // Same re-centering DexedSynth::SetPitchBend14bit() documents --
    // 8192 is wire-center, normalize to -1..1 here once rather than
    // per-sample.
    int32_t raw = (int32_t)raw14 - 8192;
    float   v   = (float)raw / 8192.f;
    pitch_bend01_    = v < -1.f ? -1.f : (v > 1.f ? 1.f : v);
    pitch_bend_rate_ = powf(2.f, (pitch_bend01_ * kPitchBendSemitones) / 12.f);
}

void SampleSynth::SetModWheel01(float v01)
{
    mod_wheel01_ = v01 < 0.f ? 0.f : (v01 > 1.f ? 1.f : v01);
}

// Called once per active voice per sample from Process() (itself ITCM) --
// same real-time-call-graph reasoning as every other ITCM tag in this
// project (see itcm.h's own comment). This one was missed when Process()
// itself was tagged -- a real, standalone out-of-line function (confirmed
// in the link map, not inlined), so every call was crossing out to
// QSPIFLASH for the actual per-voice sample read/interpolation/pitch
// advance, once per active voice, every single sample.
DSY_ITCM_TEXT
void SampleSynth::RenderVoice(Voice& v, float& out_l, float& out_r)
{
    if(v.slot < 0)
    {
        out_l = out_r = 0.f;
        return;
    }
    const SampleSlot& s = slots_[v.slot];
    if(s.len <= 0)
    {
        out_l = out_r = 0.f;
        return;
    }

    int   idx0 = (int)v.read_pos;
    float frac = v.read_pos - (float)idx0;
    if(idx0 < 0)
    {
        idx0 = 0;
        frac = 0.f;
    }
    else if(idx0 >= s.len)
    {
        idx0 = s.len - 1;
        frac = 0.f;
    }
    // Clamp, don't wrap, at the buffer's ends -- same reasoning
    // GranularEngine::RenderGrain() documents: a one-shot sample is
    // not a seamless loop, so interpolating from the last sample back
    // to the first would fabricate a real discontinuity/click.
    int idx1 = idx0 + 1 < s.len ? idx0 + 1 : idx0;
    out_l = (s.buf_l[idx0] * (1.f - frac) + s.buf_l[idx1] * frac) * s.volume01;
    out_r = (s.buf_r[idx0] * (1.f - frac) + s.buf_r[idx1] * frac) * s.volume01;

    // Global pitch bend/vibrato multiplier, recomputed once per sample
    // in Process() -- see SetPitchBend14bit()/SetModWheel01()'s own
    // comments.
    v.read_pos += v.read_inc * current_pitch_mult_;
    if(v.read_pos >= (float)s.len)
        v.read_pos = (float)s.len - 1.f;
    else if(v.read_pos < 0.f)
        v.read_pos = 0.f;
}

void SampleSynth::PlayPreview()
{
    // Always restarts from the beginning, even if a previous preview is
    // still playing -- same "tap to (re)trigger" convention
    // GranularEngine::PlayTrimPreview() already uses.
    preview_active_ = slots_[0].len > 0;
    preview_pos_     = 0.f;
}

void SampleSynth::SetAttack01(float v01)
{
    attack01_ = v01 < 0.f ? 0.f : (v01 > 1.f ? 1.f : v01);
    float seconds = kMinAdsrSeconds * powf(kMaxAdsrSeconds / kMinAdsrSeconds, attack01_);
    for(int i = 0; i < kMaxVoices; i++)
        voices_[i].adsr.SetAttackTime(seconds);
}

void SampleSynth::SetDecay01(float v01)
{
    decay01_ = v01 < 0.f ? 0.f : (v01 > 1.f ? 1.f : v01);
    float seconds = kMinAdsrSeconds * powf(kMaxAdsrSeconds / kMinAdsrSeconds, decay01_);
    for(int i = 0; i < kMaxVoices; i++)
        voices_[i].adsr.SetDecayTime(seconds);
}

void SampleSynth::SetSustain01(float v01)
{
    sustain01_ = v01 < 0.f ? 0.f : (v01 > 1.f ? 1.f : v01);
    for(int i = 0; i < kMaxVoices; i++)
        voices_[i].adsr.SetSustainLevel(sustain01_);
}

void SampleSynth::SetRelease01(float v01)
{
    release01_ = v01 < 0.f ? 0.f : (v01 > 1.f ? 1.f : v01);
    float seconds = kMinAdsrSeconds * powf(kMaxAdsrSeconds / kMinAdsrSeconds, release01_);
    for(int i = 0; i < kMaxVoices; i++)
        voices_[i].adsr.SetReleaseTime(seconds);
}

void SampleSynth::SetOutputLevel01(float v01)
{
    output_level01_ = v01 < 0.f ? 0.f : (v01 > 1.f ? 1.f : v01);
    // Same curve every other engine's own Output Level uses.
    output_level_ = powf(output_level01_, 2.5f) * 1.4f;
}

void SampleSynth::SetPan01(float v01)
{
    pan01_      = v01 < 0.f ? 0.f : (v01 > 1.f ? 1.f : v01);
    pan_l_gain_ = 1.f - pan01_;
    pan_r_gain_ = pan01_;
}

void SampleSynth::ApplyPreset(const SampleSynthPresetData& p)
{
    SetAttack01(p.attack01);
    SetDecay01(p.decay01);
    SetSustain01(p.sustain01);
    SetRelease01(p.release01);
    SetFilterMode((FilterMode)p.filter_mode);
    SetFilterCutoff01(p.filter_cutoff01);
    SetFilterResonance01(p.filter_res01);
    SetOutputLevel01(p.output_level01);
    SetPan01(p.pan01);
    SetReverbSend01(p.reverb_send01);
    SetDelaySend01(p.delay_send01);
}

SampleSynth::SampleSynthPresetData SampleSynth::CapturePreset() const
{
    SampleSynthPresetData p;
    p.attack01        = attack01_;
    p.decay01         = decay01_;
    p.sustain01       = sustain01_;
    p.release01       = release01_;
    p.filter_mode     = (int32_t)filter_mode_;
    p.filter_cutoff01 = filter_cutoff01_;
    p.filter_res01    = filter_res01_;
    p.output_level01  = output_level01_;
    p.pan01           = pan01_;
    p.reverb_send01   = reverb_send01_;
    p.delay_send01    = delay_send01_;
    return p;
}

DSY_ITCM_TEXT
void SampleSynth::Process(size_t size, float* out_l, float* out_r, float* reverb_send_l,
                            float* reverb_send_r, float* delay_send_l, float* delay_send_r)
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

    const float send       = reverb_send01_;
    const float delay_send = delay_send01_;

    for(size_t i = 0; i < size; i++)
    {
        // Pitch bend (continuous, from the wheel's own last position)
        // combined with a simple sine vibrato whose depth scales with
        // the mod wheel -- see SetPitchBend14bit()/SetModWheel01()'s
        // own comments. Recomputed every sample (not once per block)
        // since both can change continuously while notes are held.
        //
        // mod_wheel01_==0 (the common case -- most playing never
        // touches the mod wheel) is skipped entirely: sinf()/powf() are
        // real transcendental calls, not table lookups the way msfa's
        // own sin/exp2 are, so paying for them every single sample
        // regardless of whether vibrato is actually doing anything was
        // a real, measured-as-worth-fixing cost, not a style nitpick.
        // vibrato_phase_ still advances either way (cheap, one add +
        // compare) so re-engaging the mod wheel mid-note doesn't jump
        // from a stale phase.
        vibrato_phase_ += kVibratoHz / sample_rate_;
        if(vibrato_phase_ >= 1.f)
            vibrato_phase_ -= 1.f;
        if(mod_wheel01_ > 0.f)
        {
            float vibrato_semitones = sinf(vibrato_phase_ * 2.f * 3.14159265f) * mod_wheel01_
                                       * kVibratoMaxSemitones;
            current_pitch_mult_ = pitch_bend_rate_ * powf(2.f, vibrato_semitones / 12.f);
        }
        else
        {
            current_pitch_mult_ = pitch_bend_rate_;
        }

        float mix_l = 0.f, mix_r = 0.f;
        for(int vi = 0; vi < kMaxVoices; vi++)
        {
            Voice& v = voices_[vi];
            if(v.slot < 0)
                continue;
            bool gate = v.held_note >= 0;
            float env = v.adsr.Process(gate);
            if(!gate && !v.adsr.IsRunning())
            {
                v.slot = -1; // fully idle -- stop paying for this voice at all
                continue;
            }
            float vl, vr;
            RenderVoice(v, vl, vr);
            mix_l += vl * env;
            mix_r += vr * env;
        }

        // Capture/Trim's "Play" preview -- see PlayPreview()'s own
        // comment. Deliberately NOT a Voice/ADSR: a plain, ungated,
        // un-pitched one-shot readout of slot 0, mixed in alongside the
        // real voices so it still passes through the same filter/
        // output/pan/limiter chain below (what you hear on Play is
        // genuinely what Trim's selection will sound like once loaded).
        if(preview_active_)
        {
            const SampleSlot& s = slots_[0];
            if(s.len <= 0)
            {
                preview_active_ = false;
            }
            else
            {
                int   idx0 = (int)preview_pos_;
                float frac = preview_pos_ - (float)idx0;
                if(idx0 >= s.len - 1)
                {
                    idx0 = s.len > 1 ? s.len - 2 : 0;
                    frac = 1.f;
                }
                int idx1 = idx0 + 1 < s.len ? idx0 + 1 : idx0;
                mix_l += (s.buf_l[idx0] * (1.f - frac) + s.buf_l[idx1] * frac) * s.volume01;
                mix_r += (s.buf_r[idx0] * (1.f - frac) + s.buf_r[idx1] * frac) * s.volume01;
                preview_pos_ += 1.f; // always native speed -- ignores chromatic/root_note on purpose
                if(preview_pos_ >= (float)s.len)
                    preview_active_ = false;
            }
        }

        float fl = mix_l, fr = mix_r;
        if(filter_mode_ != FilterMode::Off)
        {
            filter_l_.Process(fl);
            filter_r_.Process(fr);
            switch(filter_mode_)
            {
                case FilterMode::LowPass: fl = filter_l_.Low(); fr = filter_r_.Low(); break;
                case FilterMode::HighPass: fl = filter_l_.High(); fr = filter_r_.High(); break;
                case FilterMode::BandPass: fl = filter_l_.Band(); fr = filter_r_.Band(); break;
                default: break;
            }
        }

        fl *= output_level_;
        fr *= output_level_;

        // Same PRE-limiter tap convention as DexedSynth::IsLimiting()/
        // GetPreLimiterLevel01() -- read before tanhf()/pan, see those
        // for the full reasoning.
        float pre_mag = fabsf(fl) > fabsf(fr) ? fabsf(fl) : fabsf(fr);
        if(pre_mag > pre_limiter_level_)
            pre_limiter_level_ = pre_mag;
        else
            pre_limiter_level_ -= pre_limiter_level_ * (1.f / (kMeterReleaseSeconds * sample_rate_));
        if(pre_mag >= kPreLimiterClipThreshold)
            pre_limiter_clip_hold_ = kClipHoldSeconds;
        else if(pre_limiter_clip_hold_ > 0.f)
            pre_limiter_clip_hold_ -= 1.f / sample_rate_;

        fl = tanhf(fl);
        fr = tanhf(fr);

        float sl = fl * pan_l_gain_;
        float sr = fr * pan_r_gain_;
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
