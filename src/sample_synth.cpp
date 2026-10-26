#include "sample_synth.h"
#include "pan_law.h"
#include "svf_setup.h"
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
                             uint8_t high_note, float volume01, int8_t pitch_offset_semis)
{
    if(index < 0 || index >= kMaxSampleSlots)
        return;
    SampleSlot& s = slots_[index];
    s.buf_l       = buf_l;
    s.buf_r       = buf_r;
    s.len         = len;
    s.chromatic   = chromatic;
    s.root_note   = root_note;
    s.low_note    = low_note;
    s.high_note   = high_note;
    s.volume01    = volume01;
    s.pitch_offset_semis = pitch_offset_semis;
}

void SampleSynth::SetSlotPitchOffsetSemis(int index, int8_t semis)
{
    if(index < 0 || index >= kMaxSampleSlots)
        return;
    slots_[index].pitch_offset_semis = semis;
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

    // A voice that isn't rendering anything (slot < 0) is free to take
    // outright. Idle can't mean "ADSR stopped": a one-shot holds its gate
    // open for its whole life and frees itself at the sample's end
    // (slot = -1) with the ADSR still in sustain, so once every voice had
    // played a drum pad none ever looked idle again -- and the first
    // non-held voice got stolen, cutting off whatever pad was still
    // ringing in it (confirmed on hardware: two quick hits on a Poly
    // kit, the second cut the first short).
    for(int i = 0; i < kMaxVoices; i++)
        if(voices_[i].slot < 0)
            return i;

    // Every voice is sounding: steal the oldest released one (a chromatic
    // release tail, or a one-shot whose key is up), else the oldest overall.
    int best_free = -1;
    for(int i = 0; i < kMaxVoices; i++)
    {
        if(voices_[i].held_note != -1)
            continue;
        if(best_free < 0 || voices_[i].triggered_at < voices_[best_free].triggered_at)
            best_free = i;
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
    // pitch_offset_semis is Setup page's own Tune knob -- a flat extra
    // shift added either way (0 = today's exact behavior either way,
    // powf(2,0)==1): for a one-shot slot it's the ONLY way to
    // transpose it at all (it otherwise has no note-relative pitch);
    // for a chromatic slot it's equivalent to shifting root_note, just
    // without needing to cache/restore the original root_note to
    // compute a relative shift each time the knob moves -- see
    // SetSlotPitchOffsetSemis()'s own comment.
    v.read_inc = s.chromatic
                     ? powf(2.f, ((float)note - (float)s.root_note
                                   + (float)s.pitch_offset_semis) / 12.f)
                     : powf(2.f, (float)s.pitch_offset_semis / 12.f);
    v.adsr.Retrigger(false);
    if(!s.chromatic && mono_choke_)
    {
        // See SampleSynthPresetData::mono_choke's own comment -- a new
        // pad on a break-style kit replaces whatever other one-shot
        // was still ringing, instead of layering over it. Fast-forward
        // into the SAME tail-fade window Process() already applies at
        // a one-shot's natural end (rather than freeing the slot here
        // outright) so the choked voice fades out instead of cutting
        // off with a click.
        for(int i = 0; i < kMaxVoices; i++)
        {
            if(i == vi)
                continue;
            Voice& other = voices_[i];
            if(other.slot < 0 || slots_[other.slot].chromatic)
                continue;
            int32_t other_len  = slots_[other.slot].len;
            float   fade_start = (float)other_len - OneShotFadeWindow(other_len);
            if(other.read_pos < fade_start)
                other.read_pos = fade_start;
        }
    }
    else if(s.chromatic && chromatic_mono_)
    {
        // SMPVO's own Mono/Poly -- classic "last note wins" monophonic
        // behavior, a deliberately different mechanism from mono_choke_
        // above. Same ForceRelease()-then-clear-held_note sequence as a
        // real NoteOff() (see that function's own comment) -- NOT just
        // clearing held_note, which is exactly the confirmed-on-
        // hardware stuck-note bug class that fix addresses (the gate
        // Process() samples would otherwise still read true for one
        // more block, same risk EnsembleSynth had).
        for(int i = 0; i < kMaxVoices; i++)
        {
            if(i == vi)
                continue;
            Voice& other = voices_[i];
            if(other.slot < 0 || !slots_[other.slot].chromatic)
                continue;
            other.adsr.ForceRelease();
            other.held_note = -1;
        }
    }
    __enable_irq();
}

void SampleSynth::NoteOff(uint8_t note)
{
    for(int i = 0; i < kMaxVoices; i++)
    {
        if(voices_[i].held_note == (int)note)
        {
            // One-shot/drum slots (non-chromatic) ignore note-off
            // entirely -- see Process()'s own comment on why. Calling
            // ForceRelease() here anyway would fight the gate it forces
            // true every sample right after.
            bool one_shot
                = voices_[i].slot >= 0 && !slots_[voices_[i].slot].chromatic;
            if(!one_shot)
            {
                // ForceRelease() (not just clearing held_note) is the real
                // fix for a confirmed-on-hardware stuck note found in
                // EnsembleSynth -- this engine shares the identical risk
                // (same gate-sampled daisysp::Adsr pattern). See
                // Adsr::ForceRelease()'s own doc comment for the full
                // mechanism.
                voices_[i].adsr.ForceRelease();
            }
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
        // One-shot voices ignore the ForceRelease above (their gate is
        // forced true in Process(), see its own comment) -- a real
        // panic/kit-switch needs to silence them directly too, or
        // they'd keep playing through it.
        if(voices_[i].slot >= 0 && !slots_[voices_[i].slot].chromatic)
            voices_[i].slot = -1;
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

    int   idx1 = (int)v.read_pos;
    float frac = v.read_pos - (float)idx1;
    if(idx1 < 0)
    {
        idx1 = 0;
        frac = 0.f;
    }
    else if(idx1 >= s.len)
    {
        idx1 = s.len - 1;
        frac = 0.f;
    }
    // Catmull-Rom cubic, not 2-point linear -- same upgrade, same
    // reasoning, as Resampler44_1to48::Push()'s own doc comment:
    // linear interpolation of a pitch-shifted read is a known source
    // of audible imaging, and it gets WORSE the further read_inc
    // departs from 1.0 -- confirmed on real hardware as a "crackle"
    // whose rate visibly tracked pitch (slower on low notes, faster on
    // high ones), not a fixed click, which is exactly this artifact's
    // signature rather than e.g. a decode/clipping bug. Clamp (don't
    // wrap) at both ends, same "one-shot sample isn't a seamless loop"
    // reasoning this function already applied to the old 2-point read.
    int idx0 = idx1 > 0 ? idx1 - 1 : 0;
    int idx2 = idx1 + 1 < s.len ? idx1 + 1 : idx1;
    int idx3 = idx2 + 1 < s.len ? idx2 + 1 : idx2;
    auto catmull_rom = [](float p0, float p1, float p2, float p3, float t) {
        float t2 = t * t;
        float t3 = t2 * t;
        return 0.5f
               * (2.f * p1 + (p2 - p0) * t + (2.f * p0 - 5.f * p1 + 4.f * p2 - p3) * t2
                  + (3.f * p1 - p0 - 3.f * p2 + p3) * t3);
    };
    out_l = catmull_rom(s.buf_l[idx0], s.buf_l[idx1], s.buf_l[idx2], s.buf_l[idx3], frac)
            * s.volume01;
    out_r = catmull_rom(s.buf_r[idx0], s.buf_r[idx1], s.buf_r[idx2], s.buf_r[idx3], frac)
            * s.volume01;

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
    EqualPowerPan(pan01_, &pan_l_gain_, &pan_r_gain_);
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
    SetMonoChoke(p.mono_choke);
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
    p.mono_choke      = mono_choke_;
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
            const SampleSlot& vs = slots_[v.slot];
            // Non-chromatic (drum/one-shot) slots ignore note-off's hold
            // duration entirely, same "drum pad plays its sample at
            // native pitch regardless of which key triggered it" design
            // NoteOn() already applies to pitch -- extended here to
            // duration. Without this, a brief MIDI tap (completely
            // normal for programming a drum hit) still releases this
            // voice via the default ~18ms release time almost the
            // instant it starts, truncating anything longer than a
            // very short one-shot to a near-silent snippet -- confirmed
            // as the real cause of imported drum-break slices sounding
            // "cut short"/full of gaps, not a slicing bug. gate forces
            // true here so NoteOff() (which skips ForceRelease() for
            // these slots, see its own comment) can't end them early;
            // they instead free themselves below once read_pos reaches
            // the sample's own natural end.
            bool  one_shot = !vs.chromatic;
            bool  gate     = one_shot ? true : v.held_note >= 0;
            float env      = v.adsr.Process(gate);
            if(!one_shot && !gate && !v.adsr.IsRunning())
            {
                v.slot = -1; // fully idle -- stop paying for this voice at all
                continue;
            }
            // One-shots now play all the way to the buffer's own last
            // sample instead of being cut off by note-off's release
            // (see above) -- but a real op-1 drum-break slice is cut
            // wherever the NEXT hit begins, not at a zero-crossing, so
            // its last sample is often far from silent. Stopping dead
            // there (confirmed on real hardware as an audible click at
            // the end of every closely-packed hit, e.g. a 22-slice
            // break) needs the same short forced fade a sampler would
            // apply at a one-shot's tail regardless of source content.
            // Shared with NoteOn()'s own mono_choke_ handling -- see
            // OneShotFadeWindow()'s own doc comment for why this can't
            // just be the fixed kOneShotTailFadeSamples duration.
            float tail_fade = 1.f;
            if(one_shot)
            {
                float window    = OneShotFadeWindow(vs.len);
                float remaining = (float)vs.len - v.read_pos;
                if(remaining < window)
                    tail_fade = remaining > 0.f ? remaining / window : 0.f;
            }
            float vl, vr;
            RenderVoice(v, vl, vr);
            mix_l += vl * env * tail_fade;
            mix_r += vr * env * tail_fade;
            if(one_shot && v.read_pos >= (float)vs.len - 1.f)
            {
                // Sample has fully played out (this was its last real
                // sample) -- free the voice now, independent of
                // whatever the key is still doing.
                v.slot       = -1;
                v.held_note = -1;
            }
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
