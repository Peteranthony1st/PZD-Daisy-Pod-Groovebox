#include "supersaw_synth.h"
#include "fast_math.h"
#include "pan_law.h"
#include "itcm.h"
#include <cmath>

namespace
{
// Envelopes, LFO, glide and filter coefficients run once every kEnvStep
// samples (3 kHz) and are ramped (amp) or held (the rest) in between --
// half the Ensemble's and Organ's rate, which halved this engine's
// control and saw-setup cost on the Pod (DESIGN.md, "Supersaw").
constexpr int    kEnvStep  = 16;
constexpr size_t kMaxBlock = 256;
float DSY_DTCM_BSS g_supersaw_mix[kMaxBlock];

// The JP-8000's detune table (from the decoded DSP code): the centre saw,
// then the three pairs -- slightly lopsided, as on the hardware. Each saw's
// pitch increment gets table x detune / 2^21 of the centre's added, so at
// full Detune (321) the outer pair sits about +-11% (+-190 cents) away.
constexpr int kDetuneTable[7] = {0, 128, -128, 408, -412, 704, -720};

// The Detune knob's curve, 0..127 -> 1..321 (decoded rules): +1 every
// second step to 63, +1 a step to 80, +2 a step to 120, then +8, +8, +8,
// +16, +32, +96 and the last step to 321. Slow for the first half, so most
// of the knob is the subtle-to-thick range, and the top is the wild end.
float g_detune_curve[128];

void BuildDetuneCurve()
{
    int d = 1;
    for(int i = 0; i < 128; i++)
    {
        if(i == 0)
            d = 1;
        else if(i <= 63)
            d += (i % 2 == 0) ? 1 : 0;
        else if(i <= 80)
            d += 1;
        else if(i <= 120)
            d += 2;
        else if(i <= 123)
            d += 8;
        else if(i == 124)
            d += 16;
        else if(i == 125)
            d += 32;
        else if(i == 126)
            d += 96;
        else
            d = 321;
        g_detune_curve[i] = (float)d;
    }
}

// The centre saw's fixed level on the JP (25/128); each side saw's runs
// (Mix + 1)/128 up to 33/128 (decoded Mix law: 102400 + 16384 x knob, top
// 16 bits used).
constexpr float kCentreGain = 25.f / 128.f;

// The filter's range: open is properly open (a supersaw is all top end).
constexpr float kCutoffMinHz      = 20.f;
constexpr float kCutoffMaxHz      = 20000.f;
constexpr float kFilterEnvOctaves = 7.f;
constexpr float kLfoFilterOctaves = 4.f;
constexpr float kLfoPitchSemis    = 12.f; // at full depth (depth^2 curve)
// Velocity (see SetVelSens01()): the JP's standard velocity, 80 of 127,
// plays as set; the cutoff moves up to this many octaves either side.
constexpr float kVelStandard      = 80.f / 127.f;
constexpr float kVelCutoffOctaves = 4.f;

// Fixed per-voice headroom ahead of the soft limiter, like the Ensemble's
// kVoiceGain. A starting point, to be set by ear on the hardware.
// +6.5 dB on the first setting: on the Pod it read quiet on the meter, then
// still sounded quieter than the Organ -- a supersaw's spiky peaks carry
// less energy than the same peak of sines, so it's matched by loudness.
constexpr float kVoiceGain = 0.95f;

constexpr float kPreLimiterClipThreshold = 1.f;
constexpr float kClipHoldSeconds         = 0.5f;
constexpr float kMeterReleaseSeconds     = 0.3f;

inline float Clamp01(float v)
{
    return v < 0.f ? 0.f : (v > 1.f ? 1.f : v);
}

// Saws a chunk at a time. Between wraps a saw from a phase that wraps at
// 2^32 is a straight line, so a sum of saws is one too: each chunk of up
// to kEnvStep samples starts as A + j x B (see Process()), and each saw
// that wraps inside it adds its fall of 2 from that sample on, plus a
// PolyBLEP on the samples either side to take the edge's aliasing out.
// Most chunks have no wraps at all.

// The saw's value at phase ph, -1..1 (it falls at the wrap).
inline float SawAt(uint32_t ph)
{
    return (float)(int32_t)(ph ^ 0x80000000u) * (1.f / 2147483648.f);
}

// Adds a saw's wraps to buf[1..n] (sample j is the phase after j more
// increments), at gain g. Also the PolyBLEP on the sample before a wrap
// that falls on the next chunk's first sample, since that sample is here.
// inv is 1 / inc.
inline void AddWraps(float* buf, int n, uint32_t ph, uint32_t inc, float inv, float g)
{
    if(inc == 0)
        return;
    // Samples until the wrap, as a fraction: (2^32 - ph) / inc.
    float       tau    = (ph == 0 ? 4294967296.f : (float)(uint32_t)(0u - ph)) * inv;
    const float period = 4294967296.f * inv;
    while(tau <= (float)(n + 1))
    {
        int jw = (int)tau; // the first sample after the wrap
        if((float)jw < tau)
            jw++;
        const float t = (float)jw - tau; // 0..1 past the wrap
        if(jw <= n)
        {
            buf[jw] -= g * (t + t - t * t - 1.f);
            for(int j = jw; j <= n; j++)
                buf[j] -= 2.f * g;
        }
        if(jw - 1 >= 1)
        {
            const float u = t - 1.f; // -1..0 before it
            buf[jw - 1] -= g * (u * u + u + u + 1.f);
        }
        tau += period;
    }
}

// One trapezoidal SVF stage (svf_setup.h's TptSvf, inlined with the
// coefficients passed in, so they can be shared and set every kEnvStep).
struct SvfOut
{
    float low, band, high;
};
inline SvfOut SvfStage(float in, float& ic1, float& ic2, float a1, float a2, float a3, float k)
{
    const float v3 = in - ic2;
    const float v1 = a1 * ic1 + a2 * v3;
    const float v2 = ic2 + a2 * ic1 + a3 * v3;
    ic1            = 2.f * v1 - ic1;
    ic2            = 2.f * v2 - ic2;
    return {v2, v1, in - k * v1 - v2};
}
// One chunk's per-sample work for one voice: OSC1's high-pass, OSC2
// added, the filter, the amp ramp, into the mix. A copy per filter type
// and slope, so the sample loop has no branches.
struct ChunkState
{
    float hp_x1, hp_y1, s1a, s2a, s1b, s2b, amp;
};
struct FilterCoefs
{
    float a1a, a2a, a3a, k1, a1b, a2b, a3b, kb;
};
template <int kType, bool kTwoStage>
inline void RenderChunk(const float* ss, const float* o2, float* mix, int nn, float hpc, float da,
                        const FilterCoefs& c, ChunkState& st)
{
    ChunkState z = st; // in registers for the loop
    for(int j = 1; j <= nn; j++)
    {
        const float x0 = ss[j];
        const float hp = hpc * (z.hp_y1 + x0 - z.hp_x1);
        z.hp_x1        = x0;
        z.hp_y1        = hp;
        float x        = hp + o2[j];
        if(kTwoStage)
        {
            const SvfOut a = SvfStage(x, z.s1a, z.s2a, c.a1a, c.a2a, c.a3a, c.k1);
            x = kType == 0 ? a.low : (kType == 1 ? a.high : a.band);
        }
        const SvfOut b = SvfStage(x, z.s1b, z.s2b, c.a1b, c.a2b, c.a3b, c.kb);
        const float  y = kType == 0 ? b.low : (kType == 1 ? b.high : b.band);
        z.amp += da;
        mix[j - 1] += y * z.amp;
    }
    st = z;
}

} // namespace

void SupersawSynth::Init(float sample_rate)
{
    sample_rate_ = sample_rate;
    BuildDetuneCurve();
    for(int i = 0; i < kMaxVoices; i++)
    {
        Voice& v = voices_[i];
        v.fenv.Init(sample_rate_, kEnvStep);
        v.aenv.Init(sample_rate_, kEnvStep);
        // Free-running from different places, as on the JP: no two voices
        // (and no two notes) start alike.
        for(int k = 0; k < 8; k++)
            v.phase[k] = (uint32_t)(i * 0x9E3779B9u + k * 0x7F4A7C15u);
    }
    for(int e = 0; e < kNumEnvs; e++)
    {
        SetAttack01(e, adsr_[e][0]);
        SetDecay01(e, adsr_[e][1]);
        SetSustain01(e, adsr_[e][2]);
        SetRelease01(e, adsr_[e][3]);
    }
    SetDetune01(detune01_);
    SetMix01(mix01_);
    lfo_.Init(sample_rate_, kEnvStep);

    chorus_.Init(sample_rate_);
    chorus_depth_smoothed_ = chorus_depth01_;
    chorus_.SetLfoDepth(chorus_depth_smoothed_);
    SetChorusRate01(chorus_rate01_);

    SetGain01(gain01_);
    SetOutputLevel01(output_level01_);
    SetPan01(pan01_);
}

// --- Notes -------------------------------------------------------------

// Same 3-tier shape as EnsembleSynth::FindVoiceForNote(): this note's own
// voice, else a free one (idle first), else steal the oldest.
int SupersawSynth::FindVoiceForNote(uint8_t note)
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
        const bool idle = !voices_[i].aenv.IsRunning();
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

// Points voice v at `note`. retrigger restarts both envelopes (and the
// LFO fade); glide slides from the last note played over Portamento Time.
// Called with interrupts off (see NoteOn()).
void SupersawSynth::StartVoice(Voice& v, int note, uint8_t velocity, bool retrigger, bool glide)
{
    const bool sounding = v.aenv.IsRunning();
    v.pitch_to          = (float)note;
    if(glide && last_pitch_ >= 0.f)
    {
        // A sounding mono voice glides on from wherever it is now.
        const float from  = (sounding && voice_mode_ != VoiceMode::Poly) ? v.pitch : last_pitch_;
        const float steps = (0.001f + porta_time01_ * 2.f) * sample_rate_ / (float)kEnvStep;
        v.pitch           = from;
        v.glide_step      = (v.pitch_to - from) / steps;
    }
    else
    {
        v.pitch      = v.pitch_to;
        v.glide_step = 0.f;
    }
    last_pitch_ = (float)note;
    if(retrigger)
    {
        // A legato note carries on at the first note's velocity.
        const float vn = (float)velocity * (1.f / 127.f);
        const float vs = vel_sens01_;
        v.vel_scale    = 1.f - vs * (1.f - vn);
        v.vel_amp      = 1.f - vs * (1.f - vn * vn); // the DLS/MIDI curve
        v.vel_oct      = vs * (vn - kVelStandard) * kVelCutoffOctaves;
        // Attack: about half as long at full velocity, twice at soft.
        const float att = FastExp2(vs * (kVelStandard - vn) * 2.5f);
        v.fenv.SetAttackTime(EnvSeconds(adsr_[kFilterEnv][0]) * att);
        v.aenv.SetAttackTime(EnvSeconds(adsr_[kAmpEnv][0]) * att);
        if(!sounding)
        {
            v.fade     = 0.f;
            v.amp_last = 0.f;
        }
        else if(voice_mode_ != VoiceMode::Poly)
            v.fade = 0.f; // a new mono note fades the LFO in again
        v.fenv.Retrigger(false);
        v.aenv.Retrigger(false);
    }
    v.held_note    = note;
    v.pitch_note   = note;
    v.triggered_at = ++trigger_seq_;
}

void SupersawSynth::NoteOn(uint8_t note, uint8_t velocity)
{
    __disable_irq();
    if(voice_mode_ == VoiceMode::Poly)
    {
        Voice& v = voices_[FindVoiceForNote(note)];
        StartVoice(v, note, velocity, true, porta_on_);
    }
    else
    {
        mono_vel_ = velocity;
        // Last-note priority over the keys held: this one to the top.
        int w = 0;
        for(int i = 0; i < stack_n_; i++)
            if(stack_[i] != note)
                stack_[w++] = stack_[i];
        stack_n_          = w;
        const bool others = stack_n_ > 0;
        if(stack_n_ < kStack)
            stack_[stack_n_++] = note;
        const bool retrigger = !(voice_mode_ == VoiceMode::Legato && others);
        StartVoice(voices_[0], note, velocity, retrigger, porta_on_);
    }
    __enable_irq();
}

void SupersawSynth::NoteOff(uint8_t note)
{
    __disable_irq();
    if(voice_mode_ == VoiceMode::Poly)
    {
        for(int i = 0; i < kMaxVoices; i++)
            if(voices_[i].held_note == (int)note)
            {
                // held_note first, then the release (see EnsembleSynth::NoteOff()).
                voices_[i].held_note = -1;
                voices_[i].fenv.ForceRelease();
                voices_[i].aenv.ForceRelease();
            }
    }
    else
    {
        int w = 0;
        for(int i = 0; i < stack_n_; i++)
            if(stack_[i] != note)
                stack_[w++] = stack_[i];
        stack_n_ = w;
        Voice& v = voices_[0];
        if(v.held_note == (int)note)
        {
            if(stack_n_ > 0)
                // Back to the key still held, as on Roland's mono synths:
                // Mono restarts the envelopes, Legato carries on.
                StartVoice(v, stack_[stack_n_ - 1], mono_vel_, voice_mode_ == VoiceMode::Mono,
                           porta_on_);
            else
            {
                v.held_note = -1;
                v.fenv.ForceRelease();
                v.aenv.ForceRelease();
            }
        }
    }
    __enable_irq();
}

void SupersawSynth::AllNotesOff()
{
    __disable_irq();
    stack_n_ = 0;
    for(int i = 0; i < kMaxVoices; i++)
    {
        voices_[i].held_note = -1;
        voices_[i].fenv.ForceRelease();
        voices_[i].aenv.ForceRelease();
    }
    __enable_irq();
}

void SupersawSynth::SetVoiceMode(VoiceMode m)
{
    if(m == voice_mode_)
        return;
    AllNotesOff();
    voice_mode_ = m;
}

void SupersawSynth::SetMaxVoices(int n)
{
    max_voices_ = n < 1 ? 1 : (n > kMaxVoices ? kMaxVoices : n);
    for(int i = max_voices_; i < kMaxVoices; i++)
    {
        if(voices_[i].held_note < 0)
            continue;
        voices_[i].held_note = -1;
        voices_[i].fenv.ForceRelease();
        voices_[i].aenv.ForceRelease();
    }
}

void SupersawSynth::SetPitchBend14bit(uint16_t raw14)
{
    // Every voice picks it up at its next envelope step (Process()).
    pitch_bend_ = ((float)raw14 - 8192.f) / 8192.f * 2.f;
}

void SupersawSynth::SetModWheel01(float v01)
{
    mod_wheel01_ = Clamp01(v01);
}

// --- Parameters --------------------------------------------------------

void SupersawSynth::SetDetune01(float v01)
{
    detune01_ = Clamp01(v01);
}

void SupersawSynth::UpdateDetuneRatios()
{
    float d01 = detune01_;
    if(mod_wheel_target_ == ModWheelTarget::Detune)
        d01 += mod_wheel01_ * (1.f - d01);
    const float tune2 = (float)osc2_coarse_ + osc2_fine_ * 0.01f;
    if(d01 == detune_eff_ && tune2 == osc2_eff_)
        return;
    detune_eff_   = d01;
    osc2_eff_     = tune2;
    const float x = d01 * 127.f;
    int         i = (int)x;
    if(i > 126)
        i = 126;
    const float d = g_detune_curve[i] + (g_detune_curve[i + 1] - g_detune_curve[i]) * (x - (float)i);
    for(int k = 0; k < 7; k++)
        ratio_[k] = 1.f + (float)kDetuneTable[k] * d * (1.f / 2097152.f);
    ratio_[7] = FastExp2(tune2 * (1.f / 12.f));
    for(int k = 0; k < 8; k++)
        inv_ratio_[k] = 1.f / ratio_[k];
    for(auto& v : voices_)
        v.eff_pitch = -1000.f; // every voice picks up the new ratios
}

float SupersawSynth::GetSpreadCents() const
{
    const float x = detune01_ * 127.f;
    int         i = (int)x;
    if(i > 126)
        i = 126;
    const float d = g_detune_curve[i] + (g_detune_curve[i + 1] - g_detune_curve[i]) * (x - (float)i);
    // The outer pair is the table's -720 (the slightly wider of the two).
    return -1200.f * log2f(1.f + (float)kDetuneTable[6] * d * (1.f / 2097152.f));
}

void SupersawSynth::SetMix01(float v01)
{
    mix01_     = Clamp01(v01);
    side_gain_ = (102400.f + 16384.f * mix01_ * 127.f) * (1.f / 65536.f) * (1.f / 128.f);
}

void SupersawSynth::SetOsc2Coarse(int semis)
{
    osc2_coarse_ = semis < -24 ? -24 : (semis > 24 ? 24 : semis);
}

void SupersawSynth::SetOsc2Fine(float cents)
{
    osc2_fine_ = cents < -50.f ? -50.f : (cents > 50.f ? 50.f : cents);
}

void SupersawSynth::SetBalance01(float v01)
{
    balance01_ = Clamp01(v01);
}

void SupersawSynth::SetCutoff01(float v01)
{
    cutoff01_ = Clamp01(v01);
}

void SupersawSynth::SetResonance01(float v01)
{
    res01_ = Clamp01(v01);
}

void SupersawSynth::SetFilterEnvDepth01(float v01)
{
    fenv_depth01_ = Clamp01(v01);
}

void SupersawSynth::SetKeyFollow01(float v01)
{
    key_follow01_ = Clamp01(v01);
}

float SupersawSynth::EnvSeconds(float v01)
{
    return 0.002f * powf(4.f / 0.002f, v01); // 2 ms .. 4 s, as the Ensemble
}

void SupersawSynth::SetAttack01(int env, float v01)
{
    adsr_[env][0] = Clamp01(v01);
    for(auto& v : voices_)
        (env == kFilterEnv ? v.fenv : v.aenv).SetAttackTime(EnvSeconds(adsr_[env][0]));
}

void SupersawSynth::SetDecay01(int env, float v01)
{
    adsr_[env][1] = Clamp01(v01);
    for(auto& v : voices_)
        (env == kFilterEnv ? v.fenv : v.aenv).SetDecayTime(EnvSeconds(adsr_[env][1]));
}

void SupersawSynth::SetSustain01(int env, float v01)
{
    adsr_[env][2] = Clamp01(v01);
    for(auto& v : voices_)
        (env == kFilterEnv ? v.fenv : v.aenv).SetSustainLevel(adsr_[env][2]);
}

void SupersawSynth::SetRelease01(int env, float v01)
{
    adsr_[env][3] = Clamp01(v01);
    for(auto& v : voices_)
        (env == kFilterEnv ? v.fenv : v.aenv).SetReleaseTime(EnvSeconds(adsr_[env][3]));
}

void SupersawSynth::SetVelSens01(float v01)
{
    vel_sens01_ = Clamp01(v01);
}

void SupersawSynth::SetPortaTime01(float v01)
{
    porta_time01_ = Clamp01(v01);
}

void SupersawSynth::SetChorusDepth01(float v01)
{
    chorus_depth01_ = Clamp01(v01); // Process() glides to it, as on the Ensemble
}

void SupersawSynth::SetChorusRate01(float v01)
{
    chorus_rate01_ = Clamp01(v01);
    chorus_.SetLfoFreq(0.05f + chorus_rate01_ * 1.95f); // the Ensemble's range
}

void SupersawSynth::SetGain01(float v01)
{
    gain01_ = Clamp01(v01);
    gain_   = powf(4.f, gain01_); // 0 .. +12 dB into the limiter
}

void SupersawSynth::SetOutputLevel01(float v01)
{
    output_level01_ = Clamp01(v01);
    output_level_   = powf(output_level01_, 2.5f) * 1.4f; // every engine's curve
}

void SupersawSynth::SetPan01(float v01)
{
    pan01_ = Clamp01(v01);
    EqualPowerPan(pan01_, &pan_l_gain_, &pan_r_gain_);
}

// Each saw's increment from the voice's pitch (semitones): the centre at
// the note, the side saws at the JP's detune ratios, OSC2 at its tuning.
// Also OSC1's high-pass, which sits at the note's pitch.
void SupersawSynth::SetVoiceIncrements(Voice& v, float pitch)
{
    if(pitch == v.eff_pitch)
        return; // most steps: nothing moved
    v.eff_pitch            = pitch;
    constexpr float kTwo32 = 4294967296.f;
    const float     hz     = 440.f * FastExp2((pitch - 69.f) * (1.f / 12.f));
    const float     base   = hz / sample_rate_ * kTwo32;
    const float     inv_base = 1.f / base;
    for(int k = 0; k < 8; k++)
    {
        const float inc = base * ratio_[k];
        if(inc > 2.1e9f || inc < 1.f) // above ~0.49 x the sample rate: silent
        {
            v.inc[k]     = 0;
            v.inv_inc[k] = 0.f;
            continue;
        }
        v.inc[k]     = (uint32_t)inc;
        v.inv_inc[k] = inv_base * inv_ratio_[k];
    }
    // One-pole high-pass: coefficient exp(-2 pi f / sr).
    // exp(-w) by its series, close enough for any note's w (C8 is 0.55).
    float w = 6.2831853f * hz / sample_rate_;
    w       = w > 1.f ? 1.f : w;
    v.hp_coef = 1.f - w * (1.f - w * (0.5f - w * (1.f / 6.f - w * (1.f / 24.f))));
}

// --- Audio -------------------------------------------------------------

DSY_ITCM_TEXT
void SupersawSynth::Process(size_t size, float* out_l, float* out_r, float* reverb_send_l,
                            float* reverb_send_r, float* delay_send_l, float* delay_send_r)
{
    const float meter_release = 1.f / (kMeterReleaseSeconds * sample_rate_);
    const float inv_sr        = 1.f / sample_rate_;
    if(size > kMaxBlock)
        size = kMaxBlock;

    // The LFO runs whether or not anything sounds, so a synced one stays
    // on the beat.
    const int n_steps = (int)((size + kEnvStep - 1) / kEnvStep);
    float     lfo_at[kMaxBlock / kEnvStep + 1];
    lfo_.Render(n_steps, lfo_at);

    // Silent and nothing pending: skip the voices and effects entirely.
    {
        bool any_voice = false;
        for(int vi = 0; vi < kMaxVoices && !any_voice; vi++)
            any_voice = voices_[vi].held_note >= 0 || voices_[vi].aenv.IsRunning();
        if(!any_voice && last_block_peak_ < 1e-6f)
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

    UpdateDetuneRatios();

    // Per-block settings every voice shares.
    const float cutoff01 = mod_wheel_target_ == ModWheelTarget::FilterCutoff
                               ? cutoff01_ + mod_wheel01_ * (1.f - cutoff01_)
                               : cutoff01_;
    const float base_oct  = log2f(kCutoffMinHz) + cutoff01 * log2f(kCutoffMaxHz / kCutoffMinHz);
    const float env_oct   = (fenv_depth01_ * 2.f - 1.f) * kFilterEnvOctaves;
    const float max_hz    = sample_rate_ * 0.45f;
    // Resonance: the TptSvf's curve (res^0.25), on the last stage only; at
    // 24 dB the first stage is a plain Butterworth one.
    float k_res = 2.f * (1.f - powf(res01_ * 0.95f, 0.25f));
    if(k_res < 0.05f)
        k_res = 0.05f;
    const bool  two_stage = filter_24db_;
    const float k1        = two_stage ? 1.4142136f : k_res;
    const float lfo_pitch = (lfo_.Depth(kLfoPitch));
    const float lfo_pitch_semis = lfo_pitch * (lfo_pitch < 0.f ? -lfo_pitch : lfo_pitch) * kLfoPitchSemis;
    const float lfo_filt_oct    = lfo_.Depth(kLfoFilter) * kLfoFilterOctaves;
    const float lfo_amp         = lfo_.Depth(kLfoAmp);
    const float g1 = balance01_ < 0.5f ? 1.f : 2.f * (1.f - balance01_); // OSC1
    const float g2 = balance01_ > 0.5f ? 1.f : 2.f * balance01_;         // OSC2
    const float centre_gain = kCentreGain * g1 * kVoiceGain;
    const float side_gain   = side_gain_ * g1 * kVoiceGain;
    const float osc2_gain   = g2 * 0.5f * kVoiceGain;
    const Osc2Wave   wave2  = osc2_wave_;
    const FilterType ftype  = filter_type_;

    for(size_t i = 0; i < size; i++)
        g_supersaw_mix[i] = 0.f;

    for(int vi = 0; vi < kMaxVoices; vi++)
    {
        Voice&     v    = voices_[vi];
        const bool gate = v.held_note >= 0;
        if(!gate && !v.aenv.IsRunning())
            continue;
        uint32_t ph[8];
        for(int k = 0; k < 8; k++)
            ph[k] = v.phase[k];
        float hp_x1 = v.hp_x1, hp_y1 = v.hp_y1;
        float s1a = v.f_ic1[0], s2a = v.f_ic2[0], s1b = v.f_ic1[1], s2b = v.f_ic2[1];
        for(int s = 0; s < n_steps; s++)
        {
            const size_t i0 = (size_t)s * kEnvStep;
            const size_t n  = size - i0 < (size_t)kEnvStep ? size - i0 : (size_t)kEnvStep;
            const float  lfo  = lfo_at[s];
            const float  fade = v.fade;
            v.fade = fade + lfo_.FadeStep() > 1.f ? 1.f : fade + lfo_.FadeStep();

            // Pitch: glide, bend and LFO, then the increments.
            if(v.glide_step != 0.f)
            {
                v.pitch += v.glide_step;
                if((v.glide_step > 0.f && v.pitch >= v.pitch_to)
                   || (v.glide_step < 0.f && v.pitch <= v.pitch_to))
                {
                    v.pitch      = v.pitch_to;
                    v.glide_step = 0.f;
                }
            }
            SetVoiceIncrements(v, v.pitch + pitch_bend_ + lfo * lfo_pitch_semis * fade);
            const uint32_t* inc = v.inc;
            const float*    inv = v.inv_inc;
            const float hpc = v.hp_coef;

            // Envelopes.
            const float fe     = v.fenv.Process(gate);
            const float a_from = v.amp_last;
            float       a_to   = v.aenv.Process(gate);
            // LFO to amp: full depth swings 1 -> 0 (the trance gate);
            // below centre the swing is inverted.
            const float lf = lfo_amp >= 0.f ? lfo : -lfo;
            const float la = lfo_amp >= 0.f ? lfo_amp : -lfo_amp;
            a_to *= (1.f - la * fade * (1.f - lf) * 0.5f) * v.vel_amp;
            v.amp_last     = a_to;
            const float da = (a_to - a_from) / (float)n;
            float       amp = a_from;

            // Filter coefficients for this step.
            float oct = base_oct + v.vel_oct + fe * env_oct * v.vel_scale + lfo * lfo_filt_oct * fade
                        + key_follow01_ * (v.pitch - 60.f) * (1.f / 12.f);
            float hz = FastExp2(oct);
            hz       = hz < kCutoffMinHz ? kCutoffMinHz : (hz > max_hz ? max_hz : hz);
            // g = tan(pi f / sr) as N / D (a Pade approximant, within a few
            // percent up to the 0.45 x sr clamp), never divided out: each
            // stage's a1 = 1 / (1 + g (g + k)) = D^2 / E with E = D^2 +
            // N (N + k D), and both stages share one division.
            const float x  = 3.1415927f * hz * inv_sr;
            const float x2 = x * x;
            const float N  = x * (15.f - x2);
            const float D  = 15.f - 6.f * x2;
            const float DD = D * D, ND = N * D, NN = N * N;
            const float Eb = DD + N * (N + k_res * D);
            float       inv_b, inv_a = 0.f;
            if(two_stage)
            {
                const float Ea = DD + N * (N + k1 * D);
                const float r  = 1.f / (Ea * Eb);
                inv_a          = Eb * r;
                inv_b          = Ea * r;
            }
            else
                inv_b = 1.f / Eb;
            const float a1b = DD * inv_b, a2b = ND * inv_b, a3b = NN * inv_b;
            const float a1a = DD * inv_a, a2a = ND * inv_a, a3a = NN * inv_a;

            // OSC1 and OSC2 for this chunk (index 1..n, see AddWraps()).
            float     ss[kEnvStep + 1], o2[kEnvStep + 1];
            const int nn = (int)n;
            {
                constexpr float kScale = 1.f / 2147483648.f;
                // Balance all the way to one side skips the other.
                if(centre_gain == 0.f)
                    for(int j = 1; j <= nn; j++)
                        ss[j] = 0.f;
                else
                {
                    float a = SawAt(ph[0]) * centre_gain;
                    float b = (float)inc[0] * kScale * centre_gain;
                    for(int k = 1; k < 7; k++)
                    {
                        a += SawAt(ph[k]) * side_gain;
                        b += (float)inc[k] * kScale * side_gain;
                    }
                    for(int j = 1; j <= nn; j++)
                        ss[j] = a + b * (float)j;
                    AddWraps(ss, nn, ph[0], inc[0], inv[0], centre_gain);
                    for(int k = 1; k < 7; k++)
                        AddWraps(ss, nn, ph[k], inc[k], inv[k], side_gain);
                }

                const float b2 = (float)inc[7] * kScale * osc2_gain;
                switch(osc2_gain == 0.f ? Osc2Wave::kCount : wave2)
                {
                    case Osc2Wave::Saw:
                    {
                        const float a2 = SawAt(ph[7]) * osc2_gain;
                        for(int j = 1; j <= nn; j++)
                            o2[j] = a2 + b2 * (float)j;
                        AddWraps(o2, nn, ph[7], inc[7], inv[7], osc2_gain);
                        break;
                    }
                    case Osc2Wave::Square:
                    {
                        // A saw minus the same saw half a cycle on: the
                        // slopes cancel, the two falls make the square.
                        const uint32_t half = ph[7] + 0x80000000u;
                        const float    a2   = (SawAt(ph[7]) - SawAt(half)) * osc2_gain;
                        for(int j = 1; j <= nn; j++)
                            o2[j] = a2;
                        AddWraps(o2, nn, ph[7], inc[7], inv[7], osc2_gain);
                        AddWraps(o2, nn, half, inc[7], inv[7], -osc2_gain);
                        break;
                    }
                    case Osc2Wave::kCount: // OSC2 off (Balance at OSC1)
                        for(int j = 1; j <= nn; j++)
                            o2[j] = 0.f;
                        break;
                    default: // Triangle: naive (its corners alias very little)
                        for(int j = 1; j <= nn; j++)
                        {
                            const float sw = SawAt(ph[7] + (uint32_t)j * inc[7]);
                            o2[j] = (1.f - 2.f * (sw < 0.f ? -sw : sw)) * osc2_gain;
                        }
                        break;
                }
                for(int k = 0; k < 8; k++)
                    ph[k] += (uint32_t)nn * inc[k];
            }

            float*            mix = g_supersaw_mix + i0;
            const FilterCoefs c   = {a1a, a2a, a3a, k1, a1b, a2b, a3b, k_res};
            ChunkState        cs  = {hp_x1, hp_y1, s1a, s2a, s1b, s2b, amp};
            switch((int)ftype * 2 + (two_stage ? 1 : 0))
            {
                case 0: RenderChunk<0, false>(ss, o2, mix, nn, hpc, da, c, cs); break;
                case 1: RenderChunk<0, true>(ss, o2, mix, nn, hpc, da, c, cs); break;
                case 2: RenderChunk<1, false>(ss, o2, mix, nn, hpc, da, c, cs); break;
                case 3: RenderChunk<1, true>(ss, o2, mix, nn, hpc, da, c, cs); break;
                case 4: RenderChunk<2, false>(ss, o2, mix, nn, hpc, da, c, cs); break;
                default: RenderChunk<2, true>(ss, o2, mix, nn, hpc, da, c, cs); break;
            }
            hp_x1 = cs.hp_x1;
            hp_y1 = cs.hp_y1;
            s1a   = cs.s1a;
            s2a   = cs.s2a;
            s1b   = cs.s1b;
            s2b   = cs.s2b;
        }
        for(int k = 0; k < 8; k++)
            v.phase[k] = ph[k];
        v.hp_x1    = hp_x1;
        v.hp_y1    = hp_y1;
        v.f_ic1[0] = s1a;
        v.f_ic2[0] = s2a;
        v.f_ic1[1] = s1b;
        v.f_ic2[1] = s2b;
    }

    // The shared chorus, gain, limiter, level, pan and sends -- the
    // Ensemble's output stage.
    float eff_output_level = output_level_;
    if(mod_wheel_target_ == ModWheelTarget::OutputLevel)
    {
        const float l01  = output_level01_ + mod_wheel01_ * (1.f - output_level01_);
        eff_output_level = powf(l01, 2.5f) * 1.4f;
    }
    const float depth_glide = 1.f / (0.02f * sample_rate_);
    const float send        = reverb_send01_;
    const float delay_send  = delay_send01_;
    float       block_peak  = 0.f;
    for(size_t i = 0; i < size; i++)
    {
        const float depth_was = chorus_depth_smoothed_;
        chorus_depth_smoothed_ += (chorus_depth01_ - chorus_depth_smoothed_) * depth_glide;
        if(chorus_depth_smoothed_ != depth_was)
            chorus_.SetLfoDepth(chorus_depth_smoothed_);
        float l, r;
        chorus_.Process(g_supersaw_mix[i], l, r);
        l *= gain_;
        r *= gain_;

        const float pre_mag = fabsf(l) > fabsf(r) ? fabsf(l) : fabsf(r);
        if(pre_mag > pre_limiter_level_)
            pre_limiter_level_ = pre_mag;
        else
            pre_limiter_level_ -= pre_limiter_level_ * meter_release;
        if(pre_mag >= kPreLimiterClipThreshold)
            pre_limiter_clip_hold_ = kClipHoldSeconds;
        else if(pre_limiter_clip_hold_ > 0.f)
            pre_limiter_clip_hold_ -= inv_sr;

        l = FastTanh(l) * eff_output_level;
        r = FastTanh(r) * eff_output_level;
        const float sl = l * pan_l_gain_;
        const float sr = r * pan_r_gain_;
        const float pk = fabsf(sl) > fabsf(sr) ? fabsf(sl) : fabsf(sr);
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

// --- Presets -----------------------------------------------------------

void SupersawSynth::ApplyPreset(const SupersawPresetData& p)
{
    SetDetune01(p.detune01);
    SetMix01(p.mix01);
    SetOsc2Wave((Osc2Wave)(p.osc2_wave >= 0 && p.osc2_wave < (int)Osc2Wave::kCount ? p.osc2_wave : 0));
    SetOsc2Coarse(p.osc2_coarse);
    SetOsc2Fine(p.osc2_fine);
    SetBalance01(p.balance01);
    SetFilterType(
        (FilterType)(p.filter_type >= 0 && p.filter_type < (int)FilterType::kCount ? p.filter_type : 0));
    SetFilter24dB(p.filter_24db != 0);
    SetCutoff01(p.cutoff01);
    SetResonance01(p.res01);
    SetFilterEnvDepth01(p.fenv_depth01);
    SetKeyFollow01(p.key_follow01);
    for(int e = 0; e < kNumEnvs; e++)
    {
        SetAttack01(e, p.adsr[e][0]);
        SetDecay01(e, p.adsr[e][1]);
        SetSustain01(e, p.adsr[e][2]);
        SetRelease01(e, p.adsr[e][3]);
    }
    SetLfoWave((LfoWave)(p.lfo_wave >= 0 && p.lfo_wave < (int)LfoWave::kCount ? p.lfo_wave : 0));
    SetLfoRate01(p.lfo_rate01);
    SetLfoSync(p.lfo_sync != 0);
    SetLfoSyncIndex(p.lfo_sync_index);
    SetLfoFade01(p.lfo_fade01);
    for(int d = 0; d < kNumLfoDests; d++)
        SetLfoDepth01(d, p.lfo_depth01[d]);
    SetVoiceMode(
        (VoiceMode)(p.voice_mode >= 0 && p.voice_mode < (int)VoiceMode::kCount ? p.voice_mode : 0));
    SetPortamento(p.porta_on != 0);
    SetPortaTime01(p.porta_time01);
    SetChorusDepth01(p.chorus_depth01);
    SetChorusRate01(p.chorus_rate01);
    SetGain01(p.gain01);
    SetOutputLevel01(p.output_level01);
    SetPan01(p.pan01);
    SetReverbSend01(Clamp01(p.reverb_send01));
    SetDelaySend01(Clamp01(p.delay_send01));
    if(p.mod_wheel_target >= 0 && p.mod_wheel_target < (int)ModWheelTarget::kCount)
        SetModWheelTarget((ModWheelTarget)p.mod_wheel_target);
    if(p.max_voices >= 1)
        SetMaxVoices(p.max_voices);
    SetVelSens01(p.vel_sens01);
}

SupersawSynth::SupersawPresetData SupersawSynth::CapturePreset() const
{
    SupersawPresetData p;
    p.detune01     = detune01_;
    p.mix01        = mix01_;
    p.osc2_wave    = (int32_t)osc2_wave_;
    p.osc2_coarse  = osc2_coarse_;
    p.osc2_fine    = osc2_fine_;
    p.balance01    = balance01_;
    p.filter_type  = (int32_t)filter_type_;
    p.filter_24db  = filter_24db_ ? 1 : 0;
    p.cutoff01     = cutoff01_;
    p.res01        = res01_;
    p.fenv_depth01 = fenv_depth01_;
    p.key_follow01 = key_follow01_;
    for(int e = 0; e < kNumEnvs; e++)
        for(int k = 0; k < 4; k++)
            p.adsr[e][k] = adsr_[e][k];
    p.lfo_wave       = (int32_t)lfo_.GetWave();
    p.lfo_rate01     = lfo_.GetRate01();
    p.lfo_sync       = lfo_.GetSync() ? 1 : 0;
    p.lfo_sync_index = lfo_.GetSyncIndex();
    p.lfo_fade01     = lfo_.GetFade01();
    for(int d = 0; d < kNumLfoDests; d++)
        p.lfo_depth01[d] = lfo_.GetDepth01(d);
    p.voice_mode       = (int32_t)voice_mode_;
    p.porta_on         = porta_on_ ? 1 : 0;
    p.porta_time01     = porta_time01_;
    p.chorus_depth01   = chorus_depth01_;
    p.chorus_rate01    = chorus_rate01_;
    p.gain01           = gain01_;
    p.output_level01   = output_level01_;
    p.pan01            = pan01_;
    p.reverb_send01    = reverb_send01_;
    p.delay_send01     = delay_send01_;
    p.mod_wheel_target = (int32_t)mod_wheel_target_;
    p.max_voices       = max_voices_;
    p.vel_sens01       = vel_sens01_;
    return p;
}

namespace
{
using P = SupersawSynth::SupersawPresetData;

// Each factory sound: the defaults (a classic trance supersaw) with a few
// settings changed. Amp envelope is adsr[1], the filter's adsr[0].
P Pad(P p)
{
    p.adsr[1][0] = 0.45f; // slow attack
    p.adsr[1][3] = 0.6f;  // long release
    p.chorus_depth01 = 0.5f;
    p.reverb_send01  = 0.35f;
    return p;
}

// How much each factory sound answers velocity (level and filter
// envelope): most on the plucks and stabs, least on the pads.
const float kFactoryVel[] = {0.7f, 0.3f, 0.8f, 0.5f, 0.3f, 0.6f, 0.3f, 0.8f, 0.65f, 0.5f, 0.6f, 0.3f};

// Each factory sound's Gain, so they all peak near -3 dB on the Mixer
// meter: a four-note chord at velocity 100 (one note for the mono ones),
// measured offline with the base gain of this build.
const float kFactoryGain[] = {0.18f, 0.23f, 0.74f, 0.41f, 0.14f, 0.00f, 0.39f, 0.57f,
                              0.91f, 0.00f, 0.03f, 0.00f, 0.13f, 0.25f, 0.56f, 0.62f,
                              0.47f, 0.03f, 0.22f, 0.41f, 0.17f, 0.12f, 0.14f, 0.18f,
                              0.21f, 0.00f, 0.50f, 0.81f, 0.31f, 0.52f, 0.15f, 0.19f,
                              0.04f, 0.35f, 0.00f, 0.44f, 0.00f, 0.27f, 0.00f, 0.53f,
                              0.17f, 0.78f};
constexpr int kNumGains = (int)(sizeof(kFactoryGain) / sizeof(kFactoryGain[0]));

P MakeFactory(int i)
{
    P p; // reverb send 30% unless the sound says otherwise
    p.vel_sens01    = i >= 0 && i < 12 ? kFactoryVel[i] : 0.6f;
    switch(i)
    {
        case 0: // Trance Lead: the classic, bright and wide -- the cutoff
                // left room to open further when played hard
            p.detune01 = 0.55f;
            p.mix01    = 0.75f;
            p.cutoff01 = 0.78f;
            p.res01    = 0.15f;
            break;
        case 1: // Super Pad: slow swell, the filter breathing
            p          = Pad(p);
            p.detune01 = 0.45f;
            p.cutoff01 = 0.7f;
            p.lfo_rate01 = 0.25f;
            p.lfo_depth01[SupersawSynth::kLfoFilter] = 0.62f;
            break;
        case 2: // Saw Pluck: tight and clicky -- a narrow saw, a resonant
                // filter that snaps shut, a short tail (Chord Stab is the
                // wide chord hit, Euphoric Pluck the long airy one)
            p.detune01     = 0.2f;
            p.mix01        = 0.4f;
            p.cutoff01     = 0.22f;
            p.res01        = 0.45f;
            p.fenv_depth01 = 0.9f;
            p.key_follow01 = 0.6f;
            p.adsr[0][1]   = 0.22f;
            p.adsr[0][2]   = 0.f;
            p.adsr[1][1]   = 0.3f;
            p.adsr[1][2]   = 0.f;
            p.adsr[1][3]   = 0.2f;
            p.delay_send01 = 0.25f;
            break;
        case 3: // Octave Lead: mono, legato glide, square an octave down
            p.osc2_wave    = (int32_t)SupersawSynth::Osc2Wave::Square;
            p.osc2_coarse  = -12;
            p.balance01    = 0.5f;
            p.voice_mode   = (int32_t)SupersawSynth::VoiceMode::Legato;
            p.porta_on     = 1;
            p.porta_time01 = 0.1f;
            p.lfo_rate01   = 0.55f;
            p.lfo_fade01   = 0.5f;
            p.lfo_depth01[SupersawSynth::kLfoPitch] = 0.6f;
            break;
        case 4: // Trance Gate: a pad chopped in 1/16s by the LFO
            p          = Pad(p);
            p.adsr[1][0] = 0.f;
            p.lfo_wave = (int32_t)SupersawSynth::LfoWave::Square;
            p.lfo_sync = 1;
            p.lfo_sync_index = 2; // 1/16
            p.lfo_depth01[SupersawSynth::kLfoAmp] = 1.f;
            break;
        case 5: // Anthem Stack: wide open, a saw an octave up on top
            p.detune01    = 0.7f;
            p.mix01       = 0.85f;
            p.osc2_coarse = 12;
            p.balance01   = 0.35f;
            p.cutoff01    = 0.82f;
            break;
        case 6: // Soft Strings: gentle detune, darker, slow
            p          = Pad(p);
            p.detune01 = 0.35f;
            p.mix01    = 0.5f;
            p.cutoff01 = 0.55f;
            p.chorus_depth01 = 0.65f;
            break;
        case 7: // Chord Stab: short and punchy
            p.cutoff01     = 0.45f;
            p.fenv_depth01 = 0.75f;
            p.adsr[0][1]   = 0.3f;
            p.adsr[0][2]   = 0.2f;
            p.adsr[1][1]   = 0.4f;
            p.adsr[1][2]   = 0.f;
            p.adsr[1][3]   = 0.3f;
            p.reverb_send01 = 0.3f;
            break;
        case 8: // Saw Bass: mono, a saw underneath, tight
            p.detune01     = 0.3f;
            p.mix01        = 0.5f;
            p.balance01    = 0.6f;
            p.osc2_coarse  = -12;
            p.voice_mode   = (int32_t)SupersawSynth::VoiceMode::Mono;
            p.cutoff01     = 0.4f;
            p.fenv_depth01 = 0.7f;
            p.adsr[0][1]   = 0.35f;
            p.adsr[0][2]   = 0.3f;
            p.adsr[1][3]   = 0.2f;
            p.reverb_send01 = 0.f;
            break;
        case 9: // Sub Saw: a triangle an octave down for weight
            p.osc2_wave   = (int32_t)SupersawSynth::Osc2Wave::Triangle;
            p.osc2_coarse = -12;
            p.balance01   = 0.5f;
            p.cutoff01    = 0.8f;
            break;
        case 10: // Hard Saw: everything up
            p.detune01 = 0.92f;
            p.mix01    = 1.f;
            p.cutoff01 = 0.85f;
            p.chorus_depth01 = 0.5f;
            break;
        case 12: // Big Room Stab: Roland's own supersaw-stab recipe -- both
                 // oscillators at saw, a fine detune apart, the filter
                 // envelope at full depth with a short decay, the amp held.
            p.detune01     = 0.6f;
            p.mix01        = 0.65f;
            p.osc2_fine    = 15.f;
            p.balance01    = 0.5f;
            p.cutoff01     = 0.5f;
            p.fenv_depth01 = 1.f;
            p.adsr[0][1]   = 0.28f;
            p.adsr[0][2]   = 0.f;
            p.adsr[1][3]   = 0.2f;
            p.chorus_depth01 = 0.2f;
            p.reverb_send01  = 0.3f;
            p.vel_sens01     = 0.7f;
            break;
        case 13: // Euphoric Pluck: long and airy -- wide, a saw an octave
                 // up for sparkle, a slower filter, a ringing tail into lots
                 // of delay and reverb
            p.detune01     = 0.65f;
            p.mix01        = 0.85f;
            p.osc2_coarse  = 12;
            p.balance01    = 0.3f;
            p.cutoff01     = 0.35f;
            p.res01        = 0.2f;
            p.fenv_depth01 = 0.8f;
            p.adsr[0][1]   = 0.55f;
            p.adsr[0][2]   = 0.05f;
            p.adsr[1][1]   = 0.75f;
            p.adsr[1][2]   = 0.f;
            p.adsr[1][3]   = 0.6f;
            p.chorus_depth01 = 0.55f;
            p.reverb_send01  = 0.55f;
            p.delay_send01   = 0.4f;
            p.vel_sens01     = 0.8f;
            break;
        case 14: // Saw Brass: the filter swells open, then settles
            p.detune01     = 0.3f;
            p.mix01        = 0.5f;
            p.cutoff01     = 0.4f;
            p.fenv_depth01 = 0.75f;
            p.adsr[0][0]   = 0.3f;
            p.adsr[0][1]   = 0.45f;
            p.adsr[0][2]   = 0.5f;
            p.adsr[1][0]   = 0.15f;
            p.adsr[1][3]   = 0.3f;
            p.vel_sens01   = 0.7f;
            break;
        case 15: // Reese Saw: a mono bass, wide and dark
            p.detune01     = 0.22f;
            p.mix01        = 0.9f;
            p.osc2_coarse  = -12;
            p.balance01    = 0.55f;
            p.voice_mode   = (int32_t)SupersawSynth::VoiceMode::Mono;
            p.cutoff01     = 0.35f;
            p.res01        = 0.2f;
            p.fenv_depth01 = 0.6f;
            p.adsr[0][1]   = 0.4f;
            p.adsr[0][2]   = 0.4f;
            p.adsr[1][3]   = 0.2f;
            p.reverb_send01 = 0.f;
            p.vel_sens01    = 0.6f;
            break;
        case 16: // Arp Saw: short and bright, made for the arpeggiator
            p.cutoff01     = 0.5f;
            p.fenv_depth01 = 0.7f;
            p.adsr[0][1]   = 0.3f;
            p.adsr[0][2]   = 0.25f;
            p.adsr[1][1]   = 0.5f;
            p.adsr[1][2]   = 0.3f;
            p.adsr[1][3]   = 0.3f;
            p.delay_send01 = 0.3f;
            p.vel_sens01   = 0.7f;
            break;
        case 17: // Glass Pad: a triangle an octave up for shimmer, slow
            p              = Pad(p);
            p.detune01     = 0.4f;
            p.osc2_wave    = (int32_t)SupersawSynth::Osc2Wave::Triangle;
            p.osc2_coarse  = 12;
            p.balance01    = 0.35f;
            p.cutoff01     = 0.75f;
            p.chorus_depth01 = 0.6f;
            p.vel_sens01     = 0.3f;
            break;
        case 18: // Rave Lead: mono, gliding, square an octave under
            p.detune01     = 0.85f;
            p.mix01        = 0.9f;
            p.osc2_wave    = (int32_t)SupersawSynth::Osc2Wave::Square;
            p.osc2_coarse  = -12;
            p.balance01    = 0.5f;
            p.voice_mode   = (int32_t)SupersawSynth::VoiceMode::Legato;
            p.porta_on     = 1;
            p.porta_time01 = 0.04f; // ~0.08 s: quick, just a slide in
            p.cutoff01     = 0.75f;
            p.vel_sens01   = 0.6f;
            break;
        case 19: // Unison Keys: a played keyboard sound, decaying to a hold
            p.detune01     = 0.25f;
            p.mix01        = 0.55f;
            p.cutoff01     = 0.5f;
            p.fenv_depth01 = 0.68f;
            p.adsr[0][1]   = 0.5f;
            p.adsr[0][2]   = 0.35f;
            p.adsr[1][1]   = 0.6f;
            p.adsr[1][2]   = 0.55f;
            p.adsr[1][3]   = 0.3f;
            p.key_follow01 = 0.5f;
            p.vel_sens01   = 0.75f;
            break;
        case 20: // Dark Pad: low and breathing
            p            = Pad(p);
            p.cutoff01   = 0.48f;
            p.res01      = 0.2f;
            p.lfo_rate01 = 0.2f;
            p.lfo_depth01[SupersawSynth::kLfoFilter] = 0.6f;
            p.vel_sens01 = 0.3f;
            break;
        case 21: // Shimmer Strings: a saw an octave up, lots of chorus
            p              = Pad(p);
            p.detune01     = 0.5f;
            p.osc2_coarse  = 12;
            p.balance01    = 0.3f;
            p.cutoff01     = 0.65f;
            p.chorus_depth01 = 0.75f;
            p.vel_sens01     = 0.35f;
            break;
        case 22: // Eighth Gate: the gate in 1/8s, the filter pulsing with it
            p          = Pad(p);
            p.adsr[1][0] = 0.f;
            p.cutoff01 = 0.6f;
            p.lfo_wave = (int32_t)SupersawSynth::LfoWave::Square;
            p.lfo_sync = 1;
            p.lfo_sync_index = 4; // 1/8
            p.lfo_depth01[SupersawSynth::kLfoAmp]    = 0.92f;
            p.lfo_depth01[SupersawSynth::kLfoFilter] = 0.62f;
            p.vel_sens01 = 0.4f;
            break;
        case 23: // Saw Bell: a triangle two octaves up, ringing away
            p.detune01     = 0.3f;
            p.osc2_wave    = (int32_t)SupersawSynth::Osc2Wave::Triangle;
            p.osc2_coarse  = 24;
            p.balance01    = 0.45f;
            p.cutoff01     = 0.45f;
            p.fenv_depth01 = 0.8f;
            p.adsr[0][1]   = 0.5f;
            p.adsr[0][2]   = 0.1f;
            p.adsr[1][1]   = 0.7f;
            p.adsr[1][2]   = 0.f;
            p.adsr[1][3]   = 0.6f;
            p.reverb_send01 = 0.4f;
            p.vel_sens01    = 0.7f;
            break;
        case 24: // Wobble Bass: the filter in 1/8s, resonant
            p.detune01     = 0.3f;
            p.osc2_wave    = (int32_t)SupersawSynth::Osc2Wave::Square;
            p.osc2_coarse  = -12;
            p.balance01    = 0.5f;
            p.voice_mode   = (int32_t)SupersawSynth::VoiceMode::Mono;
            p.cutoff01     = 0.42f;
            p.res01        = 0.45f;
            p.lfo_wave     = (int32_t)SupersawSynth::LfoWave::Triangle;
            p.lfo_sync     = 1;
            p.lfo_sync_index = 4; // 1/8
            p.lfo_depth01[SupersawSynth::kLfoFilter] = 0.8f;
            p.adsr[1][3]   = 0.2f;
            p.reverb_send01 = 0.f;
            p.vel_sens01    = 0.5f;
            break;
        case 25: // Riser: each note's filter climbs open over ~4 s from when
                 // it's played (the filter envelope's attack), and stays
                 // open while held. (A looping LFO sweep snapped shut at
                 // the end of each cycle, cutting held notes off.)
            p              = Pad(p);
            p.cutoff01     = 0.3f;
            p.res01        = 0.3f;
            p.fenv_depth01 = 0.95f;
            p.adsr[0][0]   = 1.f;  // 4 s rise
            p.adsr[0][2]   = 1.f;  // then stays open
            p.adsr[0][3]   = 0.6f;
            p.reverb_send01 = 0.45f;
            p.vel_sens01    = 0.2f;
            break;
        case 26: // Fifth Lead: a saw a fifth up, mono with a short glide
            p.detune01     = 0.6f;
            p.osc2_coarse  = 7;
            p.balance01    = 0.45f;
            p.voice_mode   = (int32_t)SupersawSynth::VoiceMode::Legato;
            p.porta_on     = 1;
            p.porta_time01 = 0.08f;
            p.cutoff01     = 0.75f;
            p.vel_sens01   = 0.6f;
            break;
        case 27: // Acid Saw: mono, squelchy, gliding -- the cutoff follows
                 // the keyboard, so high notes come through as well as low
            p.detune01     = 0.15f;
            p.mix01        = 0.3f;
            p.voice_mode   = (int32_t)SupersawSynth::VoiceMode::Legato;
            p.porta_on     = 1;
            p.porta_time01 = 0.1f;
            p.cutoff01     = 0.3f;
            p.res01        = 0.7f;
            p.fenv_depth01 = 0.82f;
            p.key_follow01 = 1.f;
            p.adsr[0][1]   = 0.3f;
            p.adsr[0][2]   = 0.1f;
            p.adsr[1][3]   = 0.2f;
            p.reverb_send01 = 0.f;
            p.delay_send01  = 0.2f;
            p.vel_sens01    = 0.7f;
            break;
        case 28: // Saw Choir: a vocal "aah" -- a band-pass filter for the
                 // formant, a slow swell, vibrato fading in (Soft Strings
                 // is the low-pass string pad)
            p              = Pad(p);
            p.adsr[1][0]   = 0.55f;
            p.detune01     = 0.3f;
            p.mix01        = 0.55f;
            p.filter_type  = (int32_t)SupersawSynth::FilterType::BandPass;
            p.filter_24db  = 0;
            p.cutoff01     = 0.55f;
            p.res01        = 0.4f;
            p.chorus_depth01 = 0.8f;
            p.lfo_rate01   = 0.5f;
            p.lfo_fade01   = 0.6f;
            p.lfo_depth01[SupersawSynth::kLfoPitch] = 0.55f;
            p.vel_sens01   = 0.3f;
            break;
        case 29: // Hard Stab: wide, resonant, short
            p.detune01     = 0.8f;
            p.mix01        = 0.85f;
            p.cutoff01     = 0.55f;
            p.res01        = 0.25f;
            p.fenv_depth01 = 0.8f;
            p.adsr[0][1]   = 0.32f;
            p.adsr[0][2]   = 0.15f;
            p.adsr[1][1]   = 0.42f;
            p.adsr[1][2]   = 0.f;
            p.adsr[1][3]   = 0.3f;
            p.reverb_send01 = 0.3f;
            p.vel_sens01    = 0.8f;
            break;
        case 30: // Night Drive: a slow-attack lead with delay
            p.detune01     = 0.4f;
            p.cutoff01     = 0.6f;
            p.adsr[1][0]   = 0.3f;
            p.adsr[1][3]   = 0.45f;
            p.lfo_rate01   = 0.5f;
            p.lfo_fade01   = 0.7f;
            p.lfo_depth01[SupersawSynth::kLfoPitch] = 0.56f;
            p.delay_send01 = 0.35f;
            p.vel_sens01   = 0.5f;
            break;
        case 31: // Random Steps: the filter jumping in 1/16s (Sample & Hold)
            p.cutoff01   = 0.58f;
            p.res01      = 0.35f;
            p.lfo_wave   = (int32_t)SupersawSynth::LfoWave::SampleHold;
            p.lfo_sync   = 1;
            p.lfo_sync_index = 2; // 1/16
            p.lfo_depth01[SupersawSynth::kLfoFilter] = 0.72f;
            p.delay_send01 = 0.25f;
            p.vel_sens01   = 0.5f;
            break;
        // --- styles that made the supersaw famous ----------------------
        case 32: // Future Bass: wide chords pumping on every beat, as if
                 // sidechained (a rising saw LFO on the Amp, synced 1/4)
            p.detune01     = 0.7f;
            p.mix01        = 0.85f;
            p.osc2_coarse  = 12;
            p.balance01    = 0.3f;
            p.cutoff01     = 0.72f;
            p.adsr[1][3]   = 0.4f;
            p.lfo_wave     = (int32_t)SupersawSynth::LfoWave::Saw;
            p.lfo_sync     = 1;
            p.lfo_sync_index = 6; // 1/4
            p.lfo_depth01[SupersawSynth::kLfoAmp] = 0.92f;
            p.chorus_depth01 = 0.5f;
            p.reverb_send01  = 0.35f;
            p.vel_sens01     = 0.4f;
            break;
        case 33: // Pump Pad: a pad pumping in 1/8s
            p            = Pad(p);
            p.adsr[1][0] = 0.1f;
            p.cutoff01   = 0.68f;
            p.lfo_wave   = (int32_t)SupersawSynth::LfoWave::Saw;
            p.lfo_sync   = 1;
            p.lfo_sync_index = 4; // 1/8
            p.lfo_depth01[SupersawSynth::kLfoAmp] = 0.85f;
            p.vel_sens01 = 0.3f;
            break;
        case 34: // Big Room Lead: the huge breakdown lead -- very wide, a saw
                 // an octave down, lots of reverb
            p.detune01     = 0.72f;
            p.mix01        = 0.9f;
            p.osc2_coarse  = -12;
            p.balance01    = 0.4f;
            p.cutoff01     = 0.85f;
            p.adsr[1][3]   = 0.45f;
            p.chorus_depth01 = 0.5f;
            p.reverb_send01  = 0.55f;
            p.delay_send01   = 0.3f;
            p.vel_sens01     = 0.5f;
            p.output_level01 = 0.64f; // loud even at Gain 0
            break;
        case 35: // Prog Pluck: a progressive-house pluck -- a square an
                 // octave up, into delay
            p.detune01     = 0.45f;
            p.mix01        = 0.6f;
            p.osc2_wave    = (int32_t)SupersawSynth::Osc2Wave::Square;
            p.osc2_coarse  = 12;
            p.balance01    = 0.25f;
            p.cutoff01     = 0.3f;
            p.res01        = 0.3f;
            p.fenv_depth01 = 0.85f;
            p.key_follow01 = 0.5f;
            p.adsr[0][1]   = 0.3f;
            p.adsr[0][2]   = 0.f;
            p.adsr[1][1]   = 0.42f;
            p.adsr[1][2]   = 0.f;
            p.adsr[1][3]   = 0.4f;
            p.reverb_send01 = 0.35f;
            p.delay_send01  = 0.4f;
            p.vel_sens01    = 0.75f;
            break;
        case 36: // Euro Lead: a 90s Eurodance lead -- bright, a hollow
                 // square an octave up
            p.detune01     = 0.5f;
            p.mix01        = 0.7f;
            p.osc2_wave    = (int32_t)SupersawSynth::Osc2Wave::Square;
            p.osc2_coarse  = 12;
            p.balance01    = 0.42f;
            p.cutoff01     = 0.82f;
            p.chorus_depth01 = 0.4f;
            p.vel_sens01     = 0.5f;
            p.output_level01 = 0.63f; // loud even at Gain 0
            break;
        case 37: // Hands Up Lead: a 2000s hands-up lead -- bright and wide,
                 // a quick slide between notes
            p.detune01     = 0.62f;
            p.mix01        = 0.8f;
            p.osc2_coarse  = 12;
            p.balance01    = 0.4f;
            p.voice_mode   = (int32_t)SupersawSynth::VoiceMode::Legato;
            p.porta_on     = 1;
            p.porta_time01 = 0.03f;
            p.cutoff01     = 0.85f;
            p.delay_send01 = 0.25f;
            p.vel_sens01   = 0.5f;
            break;
        case 38: // Euphoric Lead: an uplifting-trance lead -- very wide, a
                 // saw an octave up, vibrato fading in
            p.detune01     = 0.75f;
            p.mix01        = 0.85f;
            p.osc2_coarse  = 12;
            p.balance01    = 0.35f;
            p.cutoff01     = 0.8f;
            p.adsr[1][3]   = 0.45f;
            p.lfo_rate01   = 0.55f;
            p.lfo_fade01   = 0.55f;
            p.lfo_depth01[SupersawSynth::kLfoPitch] = 0.555f;
            p.reverb_send01 = 0.5f;
            p.delay_send01  = 0.35f;
            p.vel_sens01    = 0.6f;
            break;
        case 39: // Retro Brass: 80s/synthwave brass -- the filter swells
                 // open, chorus and delay
            p.detune01     = 0.25f;
            p.mix01        = 0.45f;
            p.cutoff01     = 0.42f;
            p.fenv_depth01 = 0.72f;
            p.adsr[0][0]   = 0.35f;
            p.adsr[0][1]   = 0.5f;
            p.adsr[0][2]   = 0.6f;
            p.adsr[1][0]   = 0.1f;
            p.adsr[1][3]   = 0.35f;
            p.chorus_depth01 = 0.6f;
            p.delay_send01   = 0.3f;
            p.vel_sens01     = 0.7f;
            break;
        case 40: // Retro Pad: a synthwave pad -- the filter drifting slowly,
                 // lots of chorus and reverb
            p            = Pad(p);
            p.detune01   = 0.35f;
            p.cutoff01   = 0.6f;
            p.lfo_rate01 = 0.18f;
            p.lfo_depth01[SupersawSynth::kLfoFilter] = 0.6f;
            p.chorus_depth01 = 0.75f;
            p.reverb_send01  = 0.6f;
            p.vel_sens01     = 0.3f;
            break;
        case 41: // Techno Lead: a mono melodic-techno lead -- resonant,
                 // a short glide, into delay
            p.detune01     = 0.3f;
            p.mix01        = 0.5f;
            p.voice_mode   = (int32_t)SupersawSynth::VoiceMode::Legato;
            p.porta_on     = 1;
            p.porta_time01 = 0.05f;
            p.cutoff01     = 0.42f;
            p.res01        = 0.4f;
            p.fenv_depth01 = 0.7f;
            p.key_follow01 = 0.7f;
            p.adsr[0][1]   = 0.4f;
            p.adsr[0][2]   = 0.3f;
            p.adsr[1][3]   = 0.3f;
            p.reverb_send01 = 0.3f;
            p.delay_send01  = 0.4f;
            p.vel_sens01    = 0.6f;
            break;
        default: // Slow Sweep: the filter rising and falling over two bars
            p          = Pad(p);
            p.cutoff01 = 0.6f;
            p.res01    = 0.35f;
            p.lfo_wave = (int32_t)SupersawSynth::LfoWave::Triangle;
            p.lfo_sync = 1;
            p.lfo_sync_index = 9; // 2 bars
            p.lfo_depth01[SupersawSynth::kLfoFilter] = 0.72f;
            break;
    }
    p.gain01 = kFactoryGain[i >= 0 && i < kNumGains ? i : 0];
    return p;
}

const char* const kFactoryNames[] = {"Trance Lead",   "Super Pad",      "Saw Pluck",  "Octave Lead",
                                     "Trance Gate",   "Anthem Stack",   "Soft Strings", "Chord Stab",
                                     "Saw Bass",      "Sub Saw",        "Hard Saw",   "Slow Sweep",
                                     "Big Room Stab", "Euphoric Pluck", "Saw Brass",  "Reese Saw",
                                     "Arp Saw",       "Glass Pad",      "Rave Lead",  "Unison Keys",
                                     "Dark Pad",      "Shimmer Strings", "Eighth Gate", "Saw Bell",
                                     "Wobble Bass",   "Riser",          "Fifth Lead", "Acid Saw",
                                     "Saw Choir",     "Hard Stab",      "Night Drive", "Random Steps",
                                     "Future Bass",   "Pump Pad",       "Big Room Lead", "Prog Pluck",
                                     "Euro Lead",     "Hands Up Lead",  "Euphoric Lead", "Retro Brass",
                                     "Retro Pad",     "Techno Lead"};
constexpr int kNumFactory = (int)(sizeof(kFactoryNames) / sizeof(kFactoryNames[0]));
} // namespace

int SupersawSynth::GetNumFactoryPresets()
{
    return kNumFactory;
}

const char* SupersawSynth::GetFactoryPresetName(int i)
{
    return i >= 0 && i < kNumFactory ? kFactoryNames[i] : "?";
}

SupersawSynth::SupersawPresetData SupersawSynth::GetFactoryPreset(int i)
{
    return MakeFactory(i);
}
