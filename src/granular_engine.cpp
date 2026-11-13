#include "granular_engine.h"
#include "fast_math.h"
#include "pan_law.h"
#include "svf_setup.h"
#include "itcm.h"
#include <atomic>
#include <cmath>
#include <cstdio>

using namespace daisysp;

namespace
{
inline float Clampf(float v, float lo, float hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}
constexpr float kPi = 3.14159265358979323846f;

// See GranularEngine::IsLimiting()'s own comment. Same threshold/hold
// shape as DexedSynth's own equivalent -- 1.0 matches tanhf()'s own
// documented behavior (near-transparent below unity, compresses hard
// once a peak approaches/crosses it), 0.5s hold so a one-sample peak
// between two ~30Hz UI redraws still has a real chance of being seen.
constexpr float kPreLimiterClipThreshold = 1.f;
constexpr float kClipHoldSeconds         = 0.5f;
// Same peak-meter release shape as main.cpp's own kMeterReleaseSeconds
// -- instant attack, 300ms release, so GetPreLimiterLevel01() tracks a
// visible peak instead of a symmetric follower that decays to nothing
// in ~0.27ms @ 48kHz.
constexpr float kMeterReleaseSeconds = 0.3f;

// Distributes `hits` as evenly as possible across `steps` (the common
// Bresenham-style approximation to a true Euclidean rhythm -- same
// maximally-even result as the classic examples, e.g. EuclideanMask(8,3)
// gives the 3-3-2 "tresillo" spacing). `rotate` shifts the whole pattern
// by that many steps, used to turn a pulse into an off-beat without
// needing a separate algorithm.
uint16_t EuclideanMask(int steps, int hits, int rotate)
{
    if(steps <= 0 || steps > 16)
        return 0xFFFF;
    if(hits <= 0)
        return 0;
    if(hits >= steps)
        return (uint16_t)((1u << steps) - 1);
    uint16_t mask   = 0;
    int      bucket = 0;
    for(int i = 0; i < steps; i++)
    {
        bucket += hits;
        if(bucket >= steps)
        {
            bucket -= steps;
            mask |= (uint16_t)(1u << i);
        }
    }
    rotate %= steps;
    if(rotate < 0)
        rotate += steps;
    if(rotate != 0)
    {
        uint16_t all_bits = (uint16_t)((1u << steps) - 1);
        mask = (uint16_t)(((mask << rotate) | (mask >> (steps - rotate))) & all_bits);
    }
    return mask;
}

// Tiny xorshift32 PRNG for Spray and Direction::Random.
inline uint32_t NextRandom(uint32_t& state)
{
    state ^= state << 13;
    state ^= state >> 17;
    state ^= state << 5;
    return state;
}
} // namespace


void GranularEngine::Init(float sample_rate)
{
    sample_rate_ = sample_rate;
    for(int i = 0; i < kHannTableSize; i++)
        hann_table_[i]
            = 0.5f * (1.f - cosf(2.f * kPi * (float)i / (float)(kHannTableSize - 1)));

    for(int v = 0; v < kNumVoices; v++)
        voices_[v].adsr.Init(sample_rate);
    filter_l_.Init(sample_rate);
    filter_r_.Init(sample_rate);

    SetAttack01(attack01_);
    SetDecay01(decay01_);
    SetSustain01(sustain01_);
    SetRelease01(release01_);
    SetOutputLevel01(output_level01_);
    SetDensity01(density01_);
}

void GranularEngine::SetOutputLevel01(float v01)
{
    output_level01_ = Clampf(v01, 0.f, 1.f);
    // Same shape (not the same ceiling) as LooperLayer::SetVolume01()'s
    // own curve -- deliberately diverges here. This engine's own signal
    // chain already loses real level nothing else in the project has to:
    // overlap-gain compensation (1/overlapping grains, up to -6dB+
    // depending on Size and Density) and the shared linear center-pan law (-6dB at center,
    // unavoidable without diverging that too) both eat into it before
    // this multiply even runs. Real hardware testing (user report: still
    // very quiet with Mixer's Grains channel at 100%, i.e. output_level01_
    // ==1) confirmed the old 1.4x ceiling didn't leave this knob enough
    // real headroom to compensate. Process()'s own tanhf() soft limiter
    // (added alongside this change) is what makes it safe to push this
    // much higher without harsh digital clipping once a user actually
    // turns this all the way up -- pushed again after a second real-
    // hardware report that 4x's ceiling was still too quiet: at the top
    // of the range this now drives tanhf() into real, deliberate
    // saturation (louder AND more compressed, not just louder) rather
    // than staying clean, which is the tradeoff needed to make "all the
    // way up" actually loud given how much this engine's own gain chain
    // (overlap-gain, center pan) eats before this multiply runs.
    output_level_ = powf(output_level01_, 2.5f) * 12.f;
}

void GranularEngine::SetPan01(float v01)
{
    pan01_      = Clampf(v01, 0.f, 1.f);
    EqualPowerPan(pan01_, &pan_l_gain_, &pan_r_gain_);
}

void GranularEngine::BuildShadow(const float* buf_l, const float* buf_r, size_t len)
{
    float peak = 0.f;
    for(size_t i = 0; i < len; i++)
    {
        float a = fabsf(buf_l[i]), b = fabsf(buf_r[i]);
        if(a > peak)
            peak = a;
        if(b > peak)
            peak = b;
    }
    // Peak-normalized rather than a fixed +-1.0 mapping: a capture can sit
    // above full scale (no clipping that way) or well below it (no wasted
    // bits). src_inv_scale_ folds back into each grain's gain at render.
    float scale    = peak > 0.f ? 32767.f / peak : 1.f;
    src_inv_scale_ = 1.f / scale;
    for(size_t i = 0; i < len; i++)
    {
        float l = buf_l[i] * scale, r = buf_r[i] * scale;
        shadow_[2 * i]     = (int16_t)(l + (l >= 0.f ? 0.5f : -0.5f));
        shadow_[2 * i + 1] = (int16_t)(r + (r >= 0.f ? 0.5f : -0.5f));
    }
    shadow_base_l_   = buf_l;
    shadow_base_len_ = len;
}

void GranularEngine::SetSource(const float* buf_l, const float* buf_r, size_t len)
{
    // Hide the source from Process() while the 16-bit copy is rebuilt
    // (the audio callback can preempt this at any point) -- the fence
    // stops the compiler folding this store into the final one below.
    src_len_ = 0;
    std::atomic_signal_fence(std::memory_order_seq_cst);

    // Silence every grain outright rather than letting an in-flight
    // grain's position (computed against the OLD src_len_) carry over
    // into a shorter new buffer, where it could read out of bounds.
    for(int i = 0; i < kPoolGrains; i++)
    {
        grains_[i].active = false;
        release_fades_[i] = 0.f;
    }
    for(int v = 0; v < kNumVoices; v++)
    {
        voices_[v].gate = false;
        voices_[v].adsr.ForceRelease();
    }

    if(!shadow_)
        len = 0;
    else if(len > shadow_capacity_)
        len = shadow_capacity_;
    if(len > 0)
        BuildShadow(buf_l, buf_r, len);

    src_l_   = buf_l;
    src_r_   = buf_r;
    src_s16_ = shadow_;
    std::atomic_signal_fence(std::memory_order_seq_cst);
    src_len_ = len;

    RecomputeWaveformPeaks();
    ResolveLegacyScan();
}

void GranularEngine::SetTrimRange(const float* buf_l, const float* buf_r, size_t len)
{
    // Deliberately O(1) -- no RecomputeWaveformPeaks() call here, see
    // this function's own doc comment for why that's the caller's job.
    if(shadow_base_l_ && buf_l >= shadow_base_l_
       && buf_l + len <= shadow_base_l_ + shadow_base_len_)
    {
        // Shrink to the smaller of the old/new lengths before moving the
        // start, so a callback landing mid-update never reads past either
        // range -- grains keep sounding through a live trim.
        if(len < src_len_)
            src_len_ = len;
        std::atomic_signal_fence(std::memory_order_seq_cst);
        src_l_   = buf_l;
        src_r_   = buf_r;
        src_s16_ = shadow_ + 2 * (buf_l - shadow_base_l_);
        std::atomic_signal_fence(std::memory_order_seq_cst);
        src_len_ = len;
        return;
    }

    // Outside the existing copy (not a path Trim takes) -- rebuild it.
    src_len_ = 0;
    std::atomic_signal_fence(std::memory_order_seq_cst);
    if(!shadow_)
        len = 0;
    else if(len > shadow_capacity_)
        len = shadow_capacity_;
    if(len > 0)
        BuildShadow(buf_l, buf_r, len);
    src_l_   = buf_l;
    src_r_   = buf_r;
    src_s16_ = shadow_;
    std::atomic_signal_fence(std::memory_order_seq_cst);
    src_len_ = len;
}

void GranularEngine::PlayTrimPreview()
{
    // Always restarts from the beginning, even if a previous preview is
    // still playing -- same "tap to (re)trigger" convention the preset
    // browser's own live preview already uses elsewhere in this
    // project, simplest thing that still feels responsive on repeated
    // taps. Process() reads src_l_/src_r_/src_len_ directly every
    // sample while active, so this always plays back whatever Trim
    // currently has selected, live -- there is nothing else to keep in
    // sync.
    preview_active_ = src_len_ > 0;
    preview_pos_     = 0;
}

void GranularEngine::RecomputeWaveformPeaks()
{
    if(src_len_ == 0)
    {
        for(int i = 0; i < kWaveformCols; i++)
            waveform_peaks_[i] = 0.f;
        return;
    }

    size_t per_col = src_len_ / (size_t)kWaveformCols;
    if(per_col < 1)
        per_col = 1;
    for(int col = 0; col < kWaveformCols; col++)
    {
        size_t start = (size_t)col * per_col;
        size_t end   = start + per_col;
        if(end > src_len_)
            end = src_len_;
        float peak = 0.f;
        for(size_t i = start; i < end; i++)
        {
            float a = fabsf(src_l_[i]);
            if(a > peak)
                peak = a;
        }
        waveform_peaks_[col] = peak;
    }
}

bool GranularEngine::VoiceBusy(int v) const
{
    if(voices_[v].gate || voices_[v].adsr.IsRunning())
        return true;
    for(int i = 0; i < kPoolGrains; i++)
        if(grains_[i].voice == v && (grains_[i].active || release_fades_[i] > 0.f))
            return true;
    return false;
}

bool GranularEngine::IsNoteActive() const
{
    for(int v = 0; v < kNumVoices; v++)
        if(voices_[v].gate)
            return true;
    return false;
}

void GranularEngine::SetPoly(bool on)
{
    if(on == poly_)
        return;
    poly_ = on;
    // Leaving Poly: let every voice but the first finish on its own
    // release rather than cutting it.
    if(!on)
        for(int v = 1; v < kNumVoices; v++)
            if(voices_[v].gate)
            {
                voices_[v].adsr.ForceRelease();
                voices_[v].gate = false;
            }
}

void GranularEngine::ChokeVoice(int v)
{
    // Every grain this voice owns moves to its own release-fade slot, so
    // it fades out over ~2ms instead of clicking or ringing on under the
    // voice's next note.
    for(int i = 0; i < kPoolGrains; i++)
    {
        if(!grains_[i].active || grains_[i].voice != v)
            continue;
        release_grains_[i] = grains_[i];
        release_fades_[i]  = 1.f;
        grains_[i].active  = false;
    }
}

void GranularEngine::NoteOn(uint8_t note, uint8_t velocity)
{
    const float note_rate = powf(2.f, ((float)note - 60.f) / 12.f);
    // Square-root taper, not linear (hearing is roughly logarithmic).
    const float note_gain = sqrtf((float)velocity / 127.f);
    // This runs from the MIDI timer, which the audio ISR can preempt;
    // choosing a voice and starting grains rewrites state Process() reads
    // every sample, so it happens in one go (same guard as DexedSynth).
    __disable_irq();
    // Which voice: the one already playing this key, else a free one,
    // else the oldest released one, else the oldest of all. Mono always
    // uses voice 0, choking it when the key changes.
    int vi = -1;
    if(!poly_)
    {
        vi = 0;
        if(voices_[0].note != -1 && voices_[0].note != (int)note)
            ChokeVoice(0);
    }
    else
    {
        for(int v = 0; v < kNumVoices && vi < 0; v++)
            if(voices_[v].note == (int)note && VoiceBusy(v))
                vi = v;
        for(int v = 0; v < kNumVoices && vi < 0; v++)
            if(!VoiceBusy(v))
                vi = v;
        if(vi < 0)
        {
            uint32_t oldest = 0xFFFFFFFFu;
            for(int pass = 0; pass < 2 && vi < 0; pass++)
                for(int v = 0; v < kNumVoices; v++)
                    if((pass == 1 || !voices_[v].gate) && voices_[v].triggered_at < oldest)
                    {
                        oldest = voices_[v].triggered_at;
                        vi     = v;
                    }
            ChokeVoice(vi);
        }
    }

    Voice& vc       = voices_[vi];
    bool   retrig   = vc.gate || vc.adsr.IsRunning();
    vc.note         = note;
    vc.gate         = true;
    vc.triggered_at = ++trigger_seq_;
    vc.note_rate    = note_rate;
    vc.note_gain    = note_gain;
    // Every note starts its pointer at Position (the GR-1's Scan Key Sync).
    vc.offset       = 0.f;
    vc.scan_sign    = 1.f;
    vc.rhythm_step  = 0;
    // A voice taken over mid-note restarts its attack from where its
    // envelope already is, not from zero (that would click).
    if(retrig)
        vc.adsr.Retrigger(false);

    // First grain straight away, not after the scheduler's countdown, so
    // nothing waits before it sounds. Rhythm step 0 gates it too: a
    // pattern that rests on step 0 starts genuinely silent.
    if(src_len_ > 0)
    {
        if((rhythm_mask_cached_ & 1u) != 0)
        {
            float note_mult = grain_follows_note_ ? vc.note_rate : 1.f;
            TriggerGrain(vi, PointerSamples(vc),
                         grain_tune_rate_ * note_mult * ResolveDirectionSign(), vc.note_gain);
        }
        vc.rhythm_step = 1 % kRhythmSteps;
        vc.countdown   = CurrentGrainIntervalSamples();
    }
    __enable_irq();
}

void GranularEngine::NoteOff(uint8_t note)
{
    for(int v = 0; v < kNumVoices; v++)
    {
        if(voices_[v].gate && voices_[v].note == (int)note)
        {
            // ForceRelease() (not just clearing the gate) -- a NoteOn
            // followed by this NoteOff with no audio block processed in
            // between would otherwise leave the Adsr's own gate-edge
            // detection never seeing the release (see
            // Adsr::ForceRelease()'s own doc comment).
            voices_[v].adsr.ForceRelease();
            voices_[v].gate = false;
        }
    }
}

void GranularEngine::AllNotesOff()
{
    for(int v = 0; v < kNumVoices; v++)
    {
        voices_[v].adsr.ForceRelease();
        voices_[v].gate = false;
    }
}

void GranularEngine::SetSize01(float v01)
{
    size01_ = Clampf(v01, 0.f, 1.f);
}

void GranularEngine::SetDensity01(float v01)
{
    density01_  = Clampf(v01, 0.f, 1.f);
    density_hz_ = kMinDensityHz * powf(kMaxDensityHz / kMinDensityHz, density01_);
}

// Walks Off -> 4Floor -> Tresillo -> OffBeat -> Sparse -> EclSparse ->
// EclDense -> back to Off. Discrete (the Rhythm page's Button1) rather
// than a continuous knob sweep.
void GranularEngine::CycleRhythm()
{
    rhythm_index_ = (rhythm_index_ + 1) % kNumRhythmStates;
    RecomputeRhythmPattern();
}

// Resolves rhythm_index_ into rhythm_mask_cached_/rhythm_name_cached_ --
// called only when the button actually cycles it, NOT per-hop (the
// per-hop check in Process() is just one bit test against the
// already-resolved mask). Steps are fixed at kRhythmSteps now that Speed
// (see SetGrainSpeed()) owns timing independently of Fill -- there's no
// separate "how coarse is the grid" control left to expose.
void GranularEngine::RecomputeRhythmPattern()
{
    switch(rhythm_index_)
    {
        case 0: // Off -- every hop fires, this engine's original behaviour
            rhythm_mask_cached_ = 0xFFFF;
            rhythm_name_cached_ = "Off";
            break;
        case 1: // evenly-spaced pulse
            rhythm_mask_cached_ = EuclideanMask(kRhythmSteps, kRhythmSteps / 2, 0);
            rhythm_name_cached_ = "4Floor";
            break;
        case 2: // classic 3-3-2 family
            rhythm_mask_cached_ = EuclideanMask(kRhythmSteps, kRhythmSteps * 3 / 8, 0);
            rhythm_name_cached_ = "Tresillo";
            break;
        case 3: // same pulse as 4Floor, shifted off the downbeat
            rhythm_mask_cached_ = EuclideanMask(kRhythmSteps, kRhythmSteps / 2, 1);
            rhythm_name_cached_ = "OffBeat";
            break;
        case 4: // 2 hits, maximally spread -- half-time feel
            rhythm_mask_cached_ = EuclideanMask(kRhythmSteps, 2, 0);
            rhythm_name_cached_ = "Sparse";
            break;
        case 5: // sparse Euclidean stop, denser than "Sparse" but still gappy
            rhythm_mask_cached_ = EuclideanMask(kRhythmSteps, 4, 0);
            rhythm_name_cached_ = "EclSparse";
            break;
        default: // dense Euclidean stop, just short of solid
            rhythm_mask_cached_ = EuclideanMask(kRhythmSteps, 11, 0);
            rhythm_name_cached_ = "EclDense";
            break;
    }
}

// Cycles Slow -> Medium -> Fast -> Sync -> back to Slow. Only takes
// effect once a Rhythm pattern is selected (see
// CurrentGrainIntervalSamples()) -- Fill/Gap/Size's own hop-rate
// derivation is untouched at Rhythm=Off, so nothing about today's
// continuous-texture behaviour changes for anyone who never touches
// Rhythm. Sync locks the pattern's whole kRhythmSteps-step cycle to
// exactly one bar (4 beats) of the Looper's live tempo (see
// SetExternalBpm()) -- the point of Speed existing at all is to stop
// Fill (a texture/thickness control) from secretly also being the only
// thing that decides how fast a rhythmic pattern repeats.
void GranularEngine::CycleGrainSpeed()
{
    grain_speed_ = (GrainSpeed)(((int)grain_speed_ + 1) % 4);
}

const char* GranularEngine::GetGrainSpeedName() const
{
    switch(grain_speed_)
    {
        case GrainSpeed::Slow: return "Slow";
        case GrainSpeed::Medium: return "Medium";
        case GrainSpeed::Fast: return "Fast";
        default: return "Sync";
    }
}

float GranularEngine::ComputeRhythmStepSamples() const
{
    switch(grain_speed_)
    {
        case GrainSpeed::Slow: return 0.400f * sample_rate_;
        case GrainSpeed::Medium: return 0.200f * sample_rate_;
        case GrainSpeed::Fast: return 0.100f * sample_rate_;
        default: // Sync -- one bar (4 beats) split into kRhythmSteps steps
        {
            float bpm = external_bpm_ > 1.f ? external_bpm_ : 120.f;
            return (240.f / bpm) * sample_rate_ / (float)kRhythmSteps;
        }
    }
}

float GranularEngine::CurrentGrainIntervalSamples() const
{
    return rhythm_index_ == 0 ? HopSamples() : ComputeRhythmStepSamples();
}

float GranularEngine::GrainLenSamples() const
{
    float ms = Clampf(size01_, 0.f, 1.f) * (kMaxGrainMs - kMinGrainMs) + kMinGrainMs;
    return ms * 0.001f * sample_rate_;
}

float GranularEngine::HopSamples() const
{
    return sample_rate_ / density_hz_;
}

void GranularEngine::SetPosition01(float v01)
{
    position01_ = Clampf(v01, 0.f, 1.f);
}

void GranularEngine::SetSpray01(float v01)
{
    spray01_ = Clampf(v01, 0.f, 1.f);
}

void GranularEngine::SetScan01(float v01)
{
    // Centre dead zone holds the pointer still; either side runs up to
    // 2x, squared so the slow, time-stretching speeds get most of the
    // knob (1x sits at about 70% of each side).
    legacy_scan_pending_ = false; // a knob turn (or a resolved old preset) wins
    scan01_         = Clampf(v01, 0.f, 1.f);
    float off       = scan01_ - 0.5f;
    float mag       = fabsf(off) - kScanDeadZone;
    if(mag <= 0.f)
    {
        scan_speed_ = 0.f;
        return;
    }
    float t     = mag / (0.5f - kScanDeadZone);
    scan_speed_ = (off < 0.f ? -1.f : 1.f) * t * t * kMaxScanSpeed;
}

const char* GranularEngine::GetScanModeName() const
{
    switch(scan_mode_)
    {
        case ScanMode::OneShot: return "One shot";
        case ScanMode::PingPong: return "Ping pong";
        default: return "Loop";
    }
}

void GranularEngine::SetLoopStart01(float v01)
{
    loop_start01_ = Clampf(v01, 0.f, 1.f);
}

void GranularEngine::SetLoopEnd01(float v01)
{
    loop_end01_ = Clampf(v01, 0.f, 1.f);
}

float GranularEngine::PointerSamples(const Voice& vc) const
{
    return position01_ * (float)src_len_ + vc.offset;
}

// Called from Process() (ITCM) once per sounding voice per sample.
DSY_ITCM_TEXT
void GranularEngine::AdvancePointer(Voice& vc, float lo, float hi)
{
    if(scan_speed_ == 0.f)
        return;
    float base = position01_ * (float)src_len_;
    float p    = base + vc.offset + scan_speed_ * vc.scan_sign;
    float span = hi - lo;
    if(span < 1.f)
        p = lo;
    else if(p > hi || p < lo)
    {
        bool over = p > hi;
        switch(scan_mode_)
        {
            case ScanMode::Loop: p += over ? -span : span; break;
            case ScanMode::OneShot: p = over ? hi : lo; break;
            case ScanMode::PingPong:
                p            = over ? 2.f * hi - p : 2.f * lo - p;
                vc.scan_sign = -vc.scan_sign;
                break;
        }
        p = Clampf(p, lo, hi);
    }
    vc.offset = p - base;
}

void GranularEngine::SetGrainTuneSemitones01(float v01)
{
    v01                    = Clampf(v01, 0.f, 1.f);
    grain_tune_semitones_  = (int)(v01 * 48.f + 0.5f) - 24; // -24..+24
    grain_tune_rate_       = powf(2.f, (float)grain_tune_semitones_ / 12.f);
}

float GranularEngine::GetGrainTuneSemitones01() const
{
    // SetGrainTuneSemitones01() rounds v01*48 to the nearest integer
    // before subtracting 24, so the 01 range that maps to a given
    // semitone S is [(S+23.5)/48, (S+24.5)/48) -- this must return a
    // value strictly inside that bucket, i.e. its center (S+24)/48, not
    // its exclusive upper edge. An earlier version added another +0.5
    // here (on top of the setter's own rounding), landing exactly on
    // that upper edge -- Capture/Save round-tripping this straight back
    // through the setter (see CapturePreset()/ApplyPreset()) then always
    // rounded up into the NEXT semitone, so a saved-and-reloaded Grains
    // preset always came back a semitone sharp.
    return (float)(grain_tune_semitones_ + 24) / 48.f;
}

void GranularEngine::SetDirection01(float v01)
{
    v01      = Clampf(v01, 0.f, 1.f);
    int mode = (int)(v01 * 3.f);
    if(mode > 2)
        mode = 2;
    direction_ = (Direction)mode;
}

float GranularEngine::GetDirection01() const
{
    return ((float)(int)direction_ + 0.5f) / 3.f;
}

DSY_ITCM_TEXT
float GranularEngine::Random01()
{
    return (float)(NextRandom(rng_state_) & 0x00FFFFFFu) / (float)0x00FFFFFFu;
}

float GranularEngine::ResolveDirectionSign()
{
    switch(direction_)
    {
        case Direction::Reverse: return -1.f;
        case Direction::Random: return Random01() < 0.5f ? -1.f : 1.f;
        case Direction::Forward:
        default: return 1.f;
    }
}

const char* GranularEngine::GetDirectionName() const
{
    switch(direction_)
    {
        case Direction::Reverse: return "Reverse";
        case Direction::Random: return "Random";
        default: return "Forward";
    }
}

void GranularEngine::SetAttack01(float v01)
{
    attack01_ = Clampf(v01, 0.f, 1.f);
    for(int v = 0; v < kNumVoices; v++)
        voices_[v].adsr.SetAttackTime(GetAttackSeconds());
}
void GranularEngine::SetDecay01(float v01)
{
    decay01_ = Clampf(v01, 0.f, 1.f);
    for(int v = 0; v < kNumVoices; v++)
        voices_[v].adsr.SetDecayTime(GetDecaySeconds());
}
void GranularEngine::SetSustain01(float v01)
{
    sustain01_ = Clampf(v01, 0.f, 1.f);
    for(int v = 0; v < kNumVoices; v++)
        voices_[v].adsr.SetSustainLevel(sustain01_);
}
void GranularEngine::SetRelease01(float v01)
{
    release01_ = Clampf(v01, 0.f, 1.f);
    for(int v = 0; v < kNumVoices; v++)
        voices_[v].adsr.SetReleaseTime(GetReleaseSeconds());
}
float GranularEngine::GetAttackSeconds() const
{
    return kMinAdsrSeconds * powf(kMaxAdsrSeconds / kMinAdsrSeconds, attack01_);
}
float GranularEngine::GetDecaySeconds() const
{
    return kMinAdsrSeconds * powf(kMaxAdsrSeconds / kMinAdsrSeconds, decay01_);
}
float GranularEngine::GetReleaseSeconds() const
{
    return kMinAdsrSeconds * powf(kMaxAdsrSeconds / kMinAdsrSeconds, release01_);
}

int GranularEngine::GetVoicePointers01(float* out, int max_out) const
{
    if(src_len_ == 0)
        return 0;
    int n = 0;
    for(int v = 0; v < kNumVoices && n < max_out; v++)
        if(voices_[v].gate || voices_[v].adsr.IsRunning())
            out[n++] = Clampf(PointerSamples(voices_[v]) / (float)src_len_, 0.f, 1.f);
    return n;
}

float GranularEngine::GetGrainSizeFraction01() const
{
    if(src_len_ == 0)
        return 0.f;
    return Clampf(GrainLenSamples() / (float)src_len_, 0.f, 1.f);
}

// Called from RenderGrainSpan() (itself ITCM, see its own comment below)
// once per rendered grain sample, same miss as that function's own.
DSY_ITCM_TEXT
float GranularEngine::ReadHann(float phase01) const
{
    // Nearest entry -- see kHannTableSize's own comment.
    return hann_table_[(int)(Clampf(phase01, 0.f, 1.f) * (float)(kHannTableSize - 1) + 0.5f)];
}

DSY_ITCM_TEXT
void GranularEngine::TriggerGrain(int v, float read_pos, float read_inc, float gain)
{
    // How a chord shares the pool: a free slot first; failing that, if
    // this voice holds less than its fair share (pool / voices sounding)
    // it takes the oldest grain of the voice holding the most; otherwise
    // it reuses its own oldest grain. One note alone gets all 32. A
    // stolen grain always fades out rather than cutting -- even late in
    // its window, a Hann grain on hot material is still loud enough to
    // click.
    int victim = -1;
    for(int i = 0; i < kPoolGrains && victim < 0; i++)
        if(!grains_[i].active)
            victim = i;
    if(victim < 0)
    {
        int owned[kNumVoices] = {};
        int sounding          = 0;
        for(int i = 0; i < kPoolGrains; i++)
            owned[grains_[i].voice]++;
        for(int k = 0; k < kNumVoices; k++)
            if(voices_[k].gate || voices_[k].adsr.IsRunning())
                sounding++;
        int share = kPoolGrains / (sounding > 0 ? sounding : 1);
        int from  = v;
        if(owned[v] < share)
            for(int k = 0; k < kNumVoices; k++)
                if(owned[k] > owned[from])
                    from = k;
        float worst = -1.f;
        for(int i = 0; i < kPoolGrains; i++)
            if(grains_[i].voice == from && grains_[i].phase > worst)
            {
                worst  = grains_[i].phase;
                victim = i;
            }
        if(victim < 0)
            victim = 0;
        release_grains_[victim] = grains_[victim];
        release_fades_[victim]  = 1.f;
    }

    float len   = GrainLenSamples();
    Grain& g    = grains_[victim];
    g.active    = true;
    g.voice     = (uint8_t)v;
    g.phase     = 0.f;
    g.phase_inc = len > 0.f ? 1.f / len : 1.f;
    g.read_pos  = src_len_ > 0 ? fmodf(read_pos, (float)src_len_) : 0.f;
    if(g.read_pos < 0.f && src_len_ > 0)
        g.read_pos += (float)src_len_;
    g.read_inc = read_inc;
    g.gain     = gain;
}

// Called from RenderPoolSpan() (itself ITCM, see below) once per
// grain per span -- the actual per-grain buffer read/interpolation/
// windowing, same miss as SampleSynth::RenderVoice()'s own (see its
// comment): a real, standalone out-of-line function left out when the
// callers above it were tagged. Grain state lives in locals for the whole
// span and is written back once at the end.
DSY_ITCM_TEXT
void GranularEngine::RenderGrainSpan(Grain& g, float* fade, float fade_inc, float* out_l,
                                      float* out_r, size_t count) const
{
    const int      len       = (int)src_len_;
    const int16_t* src       = src_s16_;
    float          read_pos  = g.read_pos;
    const float    read_inc  = g.read_inc;
    float          phase     = g.phase;
    const float    phase_inc = g.phase_inc;
    const float    gain      = g.gain * src_inv_scale_;
    float        f        = fade ? *fade : 1.f;

    // Fast path: how many samples this grain can run before its read
    // position nears either end of the source or its window finishes --
    // worked out once, so those samples need none of the per-sample edge
    // clamps or end checks below. One sample of margin each way absorbs
    // float rounding in the running sums. A release grain (fade != null)
    // also stops short of its fade reaching zero.
    size_t j = 0;
    if(len > 2 && phase_inc > 0.f && read_pos >= 0.f && read_pos < (float)(len - 2))
    {
        float by_phase = (1.f - phase) / phase_inc - 1.f;
        float by_read  = read_inc > 0.f   ? ((float)(len - 2) - read_pos) / read_inc - 1.f
                         : read_inc < 0.f ? read_pos / -read_inc - 1.f
                                          : (float)count;
        float safe_f   = by_phase < by_read ? by_phase : by_read;
        if(fade)
        {
            float by_fade = fade_inc > 0.f ? f / fade_inc - 1.f : (float)count;
            if(by_fade < safe_f)
                safe_f = by_fade;
        }
        size_t safe = safe_f <= 0.f ? 0 : (safe_f >= (float)count ? count : (size_t)safe_f);
        const float hann_scale = (float)(kHannTableSize - 1);
        const float f_step     = fade ? fade_inc : 0.f;
        for(; j < safe; j++)
        {
            int            idx0 = (int)read_pos;
            float          frac = read_pos - (float)idx0;
            const int16_t* p0   = src + 2 * idx0;
            float l   = (float)p0[0] + ((float)p0[2] - (float)p0[0]) * frac;
            float r   = (float)p0[1] + ((float)p0[3] - (float)p0[1]) * frac;
            float env = hann_table_[(int)(phase * hann_scale + 0.5f)] * gain * f;
            out_l[j] += l * env;
            out_r[j] += r * env;
            read_pos += read_inc;
            phase += phase_inc;
            f -= f_step;
        }
    }

    for(; j < count; j++)
    {
        int   idx0 = (int)read_pos;
        float frac = read_pos - (float)idx0;
        if(idx0 < 0)
        {
            idx0 = 0;
            frac = 0.f;
        }
        else if(idx0 >= len)
        {
            idx0 = len - 1;
            frac = 0.f;
        }
        // Clamp, don't wrap, at the buffer's last sample -- src_l_/src_r_
        // isn't guaranteed to be a seamless loop (a captured loop layer
        // usually is, by construction, but an imported one-shot sample
        // almost never is), so interpolating straight from the last sample
        // back to sample 0 here would fabricate a real discontinuity right
        // in the middle of a grain's read, past the Hann window's own
        // fade-out at the grain's actual start/end -- audible as a click,
        // worse the more of the buffer a grain (or Scan, sweeping its
        // anchor close to both ends) reads through.
        int   idx1 = idx0 + 1 < len ? idx0 + 1 : idx0;
        const int16_t* p0 = src + 2 * idx0;
        const int16_t* p1 = src + 2 * idx1;
        float l    = (float)p0[0] + ((float)p1[0] - (float)p0[0]) * frac;
        float r    = (float)p0[1] + ((float)p1[1] - (float)p0[1]) * frac;
        float env  = ReadHann(phase) * gain * f;

        out_l[j] += l * env;
        out_r[j] += r * env;

        read_pos += read_inc;
        // Same clamp-not-wrap reasoning as idx1 above, for the grain's
        // ongoing read position -- a grain that runs past either end just
        // holds at that edge (silent-ish anyway, this close to the buffer
        // boundary and to the grain's own natural end) instead of jumping to
        // the opposite end of a buffer that was never meant to loop.
        if(read_pos >= (float)len)
            read_pos = (float)len - 1.f;
        else if(read_pos < 0.f)
            read_pos = 0.f;
        phase += phase_inc;
        if(fade)
        {
            f -= fade_inc;
            if(f < 0.f)
                f = 0.f;
        }
        if(phase >= 1.f)
        {
            g.active = false;
            // A finished release grain's window is already at 0, so its
            // remaining fade would only render silence -- free the slot.
            f = 0.f;
            break;
        }
        if(fade && f <= 0.f)
            break;
    }

    g.read_pos = read_pos;
    g.phase    = phase;
    if(fade)
        *fade = f;
}

// Called from Process() (itself ITCM) for each span between trigger
// points -- same missed-callee pattern as RenderGrainSpan()'s own comment.
DSY_ITCM_TEXT
void GranularEngine::RenderPoolSpan(size_t from, size_t to)
{
    if(to <= from)
        return;
    const size_t count    = to - from;
    const float  kFadeInc = 1.f / (0.002f * sample_rate_);
    for(int i = 0; i < kPoolGrains; i++)
    {
        int v = grains_[i].voice;
        if(grains_[i].active)
            RenderGrainSpan(grains_[i], nullptr, 0.f, chunk_l_[v] + from, chunk_r_[v] + from,
                            count);
        if(release_fades_[i] > 0.f)
        {
            int rv = release_grains_[i].voice;
            RenderGrainSpan(release_grains_[i], &release_fades_[i], kFadeInc, chunk_l_[rv] + from,
                            chunk_r_[rv] + from, count);
        }
    }
}

DSY_ITCM_TEXT
void GranularEngine::Process(size_t size, float* out_l, float* out_r, float* reverb_send_l,
                              float* reverb_send_r, float* delay_send_l, float* delay_send_r)
{
    // Per-block constants (were divisions every sample).
    const float meter_release = 1.f / (kMeterReleaseSeconds * sample_rate_);
    const float inv_sr        = 1.f / sample_rate_;

    // Which voices have anything to do this block (a held key, an
    // envelope still releasing, or a grain still playing/fading).
    bool busy[kNumVoices];
    int  sounding = 0;
    bool any_busy = false;
    for(int v = 0; v < kNumVoices; v++)
    {
        busy[v] = VoiceBusy(v);
        any_busy |= busy[v];
        if(voices_[v].gate || voices_[v].adsr.IsRunning())
            sounding++;
    }

    // Silent and nothing pending: skip all the per-sample work. Output is
    // plain zero, the sends get nothing, and the meters decay as they
    // would have.
    if(!any_busy && !preview_active_ && last_block_peak_ < 1e-6f)
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
    float block_peak = 0.f;
    bool has_source = src_len_ > 0;

    float cutoff_hz = kFilterMinHz * powf(kFilterMaxHz / kFilterMinHz, filter_cutoff01_);
    float nyquist_guard = sample_rate_ / 3.f - 1.f;
    cutoff_hz = cutoff_hz < kFilterMinHz ? kFilterMinHz
                : cutoff_hz > nyquist_guard ? nyquist_guard
                                            : cutoff_hz;
    filter_l_.SetFreq(cutoff_hz);
    filter_l_.SetRes(filter_res01_ * 0.9f);
    filter_r_.SetFreq(cutoff_hz);
    filter_r_.SetRes(filter_res01_ * 0.9f);

    // Overlap-add gain compensation: grains overlapping at once (Size x
    // the grain rate, at most the pool) would otherwise add up linearly.
    // Full 1/n, not sqrt -- sqrt still let loudness creep up as density
    // rose.
    float overlap = GrainLenSamples() / CurrentGrainIntervalSamples();
    overlap       = Clampf(overlap, 1.f, (float)kPoolGrains);
    const float overlap_gain = grain_volume01_ / overlap;

    // Chord headroom target, see chord_scale_.
    const float chord_target = sounding > 1 ? 1.f / sqrtf((float)sounding) : 1.f;
    const float chord_step   = 1.f / (0.008f * sample_rate_);

    // Pointer range (Loop Start/End) -- min/max so Start above End still
    // makes a correct range.
    float la      = loop_start01_ * (float)src_len_;
    float lb      = loop_end01_ * (float)src_len_;
    float loop_lo = fminf(la, lb);
    float loop_hi = fmaxf(la, lb);
    if(loop_hi > (float)src_len_ - 1.f)
        loop_hi = (float)src_len_ - 1.f;
    const float spray_samples = GetSprayMs() * 0.001f * sample_rate_;

    for(size_t base = 0; base < size; base += kRenderChunk)
    {
        const size_t n = size - base < kRenderChunk ? size - base : kRenderChunk;
        for(int v = 0; v < kNumVoices; v++)
            for(size_t j = 0; j < n; j++)
            {
                chunk_l_[v][j]   = 0.f;
                chunk_r_[v][j]   = 0.f;
                chunk_env_[v][j] = 0.f;
            }

        if(has_source)
        {
            size_t done = 0;
            for(size_t j = 0; j < n; j++)
            {
                for(int v = 0; v < kNumVoices; v++)
                {
                    if(!busy[v])
                        continue;
                    Voice& vc        = voices_[v];
                    chunk_env_[v][j] = vc.adsr.Process(vc.gate);
                    // Grains keep spawning through the WHOLE release tail,
                    // not just while the key is held -- otherwise a long
                    // Release has nothing new to shape once the grains in
                    // flight at NoteOff finish.
                    if(!(vc.gate || vc.adsr.IsRunning()))
                        continue;
                    AdvancePointer(vc, loop_lo, loop_hi);
                    vc.countdown -= 1.f;
                    if(vc.countdown > 0.f)
                        continue;
                    // Rhythm gate: the step clock advances on every hop,
                    // but only a step set in the pattern spawns a grain.
                    bool fires     = ((rhythm_mask_cached_ >> vc.rhythm_step) & 1u) != 0;
                    vc.rhythm_step = (vc.rhythm_step + 1) % kRhythmSteps;
                    if(fires)
                    {
                        float pos = PointerSamples(vc);
                        if(spray_samples > 0.f)
                            pos += (Random01() * 2.f - 1.f) * spray_samples;
                        float note_mult = grain_follows_note_ ? vc.note_rate : 1.f;
                        RenderPoolSpan(done, j);
                        done = j;
                        TriggerGrain(v, pos, grain_tune_rate_ * note_mult * ResolveDirectionSign(),
                                     vc.note_gain);
                    }
                    vc.countdown += CurrentGrainIntervalSamples();
                }
            }
            RenderPoolSpan(done, n);
        }

        for(size_t j = 0; j < n; j++)
        {
            const size_t i = base + j;
            float mixed_l = 0.f, mixed_r = 0.f;
            for(int v = 0; v < kNumVoices; v++)
            {
                if(!busy[v])
                    continue;
                mixed_l += chunk_l_[v][j] * chunk_env_[v][j];
                mixed_r += chunk_r_[v][j] * chunk_env_[v][j];
            }
            if(chord_scale_ < chord_target)
                chord_scale_ = fminf(chord_scale_ + chord_step, chord_target);
            else if(chord_scale_ > chord_target)
                chord_scale_ = fmaxf(chord_scale_ - chord_step, chord_target);
            mixed_l *= overlap_gain * chord_scale_;
            mixed_r *= overlap_gain * chord_scale_;

            // Safety backstop -- overlap and chord compensation handle the
            // common case, but release fades and a hot source can still
            // add up past full scale. A cheap hard clamp, since this runs
            // every sample.
            mixed_l = Clampf(mixed_l, -1.f, 1.f);
            mixed_r = Clampf(mixed_r, -1.f, 1.f);

            float fl = mixed_l, fr = mixed_r;
            if(filter_mode_ != FilterMode::Off)
            {
                filter_l_.Process(mixed_l);
                filter_r_.Process(mixed_r);
                switch(filter_mode_)
                {
                    case FilterMode::LowPass: fl = filter_l_.Low(); fr = filter_r_.Low(); break;
                    case FilterMode::HighPass: fl = filter_l_.High(); fr = filter_r_.High(); break;
                    case FilterMode::BandPass: fl = filter_l_.Band(); fr = filter_r_.Band(); break;
                    default: break;
                }
            }

            fl *= output_level_ * gain_ * pan_l_gain_;
            fr *= output_level_ * gain_ * pan_r_gain_;

            // Trim preview (see PlayTrimPreview()'s own comment), injected
            // here -- AFTER output_level_/pan, not mixed into mixed_l/
            // mixed_r earlier -- on purpose. output_level_ can scale up to
            // 12x (see SetOutputLevel01()'s own comment), tuned to
            // compensate for how quiet a REAL grain signal is after Hann-
            // window overlap-gain division and grain_volume01_; a real
            // hardware bug confirmed this previously
            // added the raw, unattenuated source sample into mixed_l/r
            // BEFORE that multiply, so it got hit by the same up-to-12x
            // compensation a real (far quieter) grain signal needs, playing
            // back extremely loud even after a first attempt to fix it by
            // scaling down by grain_volume01_ alone -- nowhere near enough
            // against a 12x multiplier. Added here instead, with its own
            // small fixed gain completely independent of output_level_/pan/
            // the filter (previewing the raw trim selection, not a filtered
            // or panned version of it), so its loudness stays predictable
            // regardless of how Output Level or Grain Volume happen to be
            // set. Still runs through the pre-limiter tracking and FastTanh()
            // safety net just below, same as everything else.
            constexpr float kPreviewGain = 0.5f;
            if(preview_active_)
            {
                if(preview_pos_ < src_len_)
                {
                    fl += src_l_[preview_pos_] * kPreviewGain;
                    fr += src_r_[preview_pos_] * kPreviewGain;
                    preview_pos_++;
                }
                else
                {
                    preview_active_ = false;
                }
            }

            // See IsLimiting()'s/GetPreLimiterLevel01()'s own comments --
            // both read the PRE-limiter value specifically, since FastTanh()
            // below bounds its own output to (-1,1) no matter how hard
            // this is driving it.
            float pre_limiter_mag = fabsf(fl) > fabsf(fr) ? fabsf(fl) : fabsf(fr);
            if(pre_limiter_mag > pre_limiter_level_)
                pre_limiter_level_ = pre_limiter_mag;
            else
                pre_limiter_level_
                    -= pre_limiter_level_ * meter_release;
            if(pre_limiter_mag >= kPreLimiterClipThreshold)
                pre_limiter_clip_hold_ = kClipHoldSeconds;
            else if(pre_limiter_clip_hold_ > 0.f)
                pre_limiter_clip_hold_ -= inv_sr;

            // Soft limiter -- output_level_'s own ceiling was raised well past
            // unity (see SetOutputLevel01()'s comment) specifically so the
            // Output Level knob has enough real headroom to compensate for
            // this engine's own overlap-gain and center-pan attenuation, which
            // Dexed/the loop layers don't have -- this is what makes pushing
            // that knob all the way up loud instead of just clipping. FastTanh()
            // is near-transparent at normal levels and only compresses once a
            // peak actually approaches/exceeds unity, same convention as
            // DexedSynth::Process()'s own limiter.
            fl = FastTanh(fl);
            fr = FastTanh(fr);
            float pk = fabsf(fl) > fabsf(fr) ? fabsf(fl) : fabsf(fr);
            if(pk > block_peak)
                block_peak = pk;

            out_l[i] = fl;
            out_r[i] = fr;
            reverb_send_l[i] += fl * reverb_send01_;
            reverb_send_r[i] += fr * reverb_send01_;
            delay_send_l[i] += fl * delay_send01_;
            delay_send_r[i] += fr * delay_send01_;
        }
    }
    last_block_peak_ = block_peak;
}

void GranularEngine::ApplyPreset(const GranularPresetData& p)
{
    SetSize01(p.size01);
    SetPosition01(p.position01);
    SetLoopStart01(p.scan_start01);
    SetLoopEnd01(p.scan_end01);
    SetGrainTuneSemitones01(p.grain_tune01);
    SetGrainFollowsNote(p.grain_follows_note);
    SetDirection01(p.direction01);
    SetGrainVolume01(p.grain_volume01);

    if(p.density01 >= 0.f)
    {
        SetDensity01(p.density01);
        SetSpray01(p.spray01 >= 0.f ? p.spray01 : 0.f);
        SetScan01(p.scan_speed01 >= 0.f ? p.scan_speed01 : 0.5f);
        scan_mode_ = p.scan_mode >= 0 ? (ScanMode)(p.scan_mode % 3) : ScanMode::Loop;
    }
    else
    {
        // A two-cloud preset. If its moving Scan cloud was audible and at
        // least as loud as the fixed Grain cloud, the one cloud takes the
        // Scan cloud's part (it moves); otherwise it keeps the Grain
        // cloud's (it holds at Position).
        const float off   = p.scan01 - 0.5f;
        const bool  moved = fabsf(off) > 0.09f && p.scan_volume01 > 0.001f
                           && p.scan_volume01 >= p.grain_volume01;
        float fill = moved ? p.scan_fill01 * 10.f : p.fill01 * 16.f;
        float gap  = moved ? p.scan_gap01 : p.gap01;
        // Old interval: grain length / fill, stretched by up to 7x by Gap.
        float len  = GrainLenSamples();
        float hop  = (fill >= 1.f ? len / fill : len) * (1.f + gap * 6.f);
        float hz   = hop > 1.f ? sample_rate_ / hop : kMaxDensityHz;
        hz         = Clampf(hz, kMinDensityHz, kMaxDensityHz);
        SetDensity01(logf(hz / kMinDensityHz) / logf(kMaxDensityHz / kMinDensityHz));
        // Old Jitter: up to one grain length either side.
        float spray_ms = p.jitter01 * (len / sample_rate_) * 1000.f;
        SetSpray01(sqrtf(Clampf(spray_ms / kMaxSprayMs, 0.f, 1.f)));
        if(moved)
        {
            SetGrainVolume01(p.scan_volume01);
            SetPosition01(p.scan_start01);
            direction_ = (Direction)(p.scan_direction % 3);
            scan_mode_ = p.scan_bounce ? ScanMode::PingPong : ScanMode::Loop;
            // Old speed: up to 1 buffer per second, the same either side
            // of its dead zone.
            float t             = (fabsf(off) - 0.09f) / (0.5f - 0.09f);
            legacy_scan_frac_   = (off < 0.f ? -1.f : 1.f) * Clampf(t, 0.f, 1.f);
            legacy_scan_pending_ = true;
            scan01_             = 0.5f;
            scan_speed_         = 0.f;
            ResolveLegacyScan();
        }
        else
        {
            SetScan01(0.5f);
            scan_mode_ = p.scan_bounce ? ScanMode::PingPong : ScanMode::Loop;
        }
    }

    SetPoly(p.poly != 0);
    SetAttack01(p.attack01);
    SetDecay01(p.decay01);
    SetSustain01(p.sustain01);
    SetRelease01(p.release01);
    SetFilterMode((FilterMode)p.filter_mode);
    SetFilterCutoff01(p.filter_cutoff01);
    SetFilterResonance01(p.filter_res01);
    rhythm_index_ = p.rhythm_index % kNumRhythmStates;
    RecomputeRhythmPattern();
    grain_speed_ = (GrainSpeed)(p.grain_speed % 4);
    SetReverbSend01(p.reverb_send01);
    if(p.output_level01 >= 0.f)
        SetOutputLevel01(p.output_level01);
    if(p.pan01 >= 0.f)
        SetPan01(p.pan01);
    if(p.gain01 >= 0.f)
        SetGain01(p.gain01);
    if(p.delay_send01 >= 0.f)
        SetDelaySend01(p.delay_send01);
}

void GranularEngine::ResolveLegacyScan()
{
    if(!legacy_scan_pending_ || src_len_ == 0)
        return;
    // Buffer fraction per second -> samples per sample, then back onto
    // the Scan knob (see SetScan01()'s curve).
    float x   = Clampf(legacy_scan_frac_ * (float)src_len_ / sample_rate_, -kMaxScanSpeed,
                       kMaxScanSpeed);
    float t   = sqrtf(fabsf(x) / kMaxScanSpeed);
    float v01 = 0.5f + (x < 0.f ? -1.f : 1.f) * (kScanDeadZone + t * (0.5f - kScanDeadZone));
    SetScan01(v01); // also clears legacy_scan_pending_
}

GranularEngine::GranularPresetData GranularEngine::CapturePreset() const
{
    GranularPresetData p;
    p.size01              = size01_;
    p.position01          = position01_;
    p.scan_start01        = loop_start01_;
    p.scan_end01          = loop_end01_;
    p.grain_tune01        = GetGrainTuneSemitones01();
    p.grain_follows_note  = grain_follows_note_;
    p.direction01         = GetDirection01();
    p.attack01            = attack01_;
    p.decay01             = decay01_;
    p.sustain01           = sustain01_;
    p.release01           = release01_;
    p.filter_mode         = (int32_t)filter_mode_;
    p.filter_cutoff01     = filter_cutoff01_;
    p.filter_res01        = filter_res01_;
    p.grain_volume01      = grain_volume01_;
    p.rhythm_index        = rhythm_index_;
    p.grain_speed         = (int32_t)grain_speed_;
    p.reverb_send01       = reverb_send01_;
    p.output_level01      = output_level01_;
    p.pan01               = pan01_;
    p.gain01              = gain01_;
    p.delay_send01        = delay_send01_;
    p.density01           = density01_;
    p.spray01             = spray01_;
    p.scan_speed01        = scan01_;
    p.scan_mode           = (int32_t)scan_mode_;
    p.poly                = poly_ ? 1 : 0;
    return p;
}
