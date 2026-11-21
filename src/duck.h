#pragma once
#include <cmath>
#include <cstddef>
#include <cstdint>

// Sidechain ducking (Global:Duck, Global:DuckEnv, Global:DuckAmt): one
// source makes any number of mixer channels dip and swell back, each by
// its own amount.
//
// A note source (a MIDI track, or an engine) triggers the dip on every
// note it plays -- or only on the chosen notes, e.g. a kit's kick pads. An
// audio source (an audio track, or the input) triggers it when its low end
// crosses a threshold, so a recorded break pumps on its kick. Either way
// the dip has the same shape: a short ramp down, Hold, then Release.
//
// A ducked channel's reverb and delay sends duck with it, like a
// compressor on that channel.
namespace Duck
{
// The channels, the same cells as Global:Sends: the four audio tracks,
// then the engines (DXD-A and DXD-B share the Dexed bus), then Bypass.
constexpr int kChannels = 12;
enum Channel
{
    kA1  = 0, // .. kA1 + 3
    kGr  = 4,
    kDxd = 5,
    kEns = 6,
    kOrg = 7,
    kSsw = 8,
    kSmv = 9,
    kSmd = 10,
    kByp = 11,
};

// Sources, as stored: M1..M8, then the engines (MidiSequencer::Engine
// order), then A1..A4, then the input.
constexpr int kSrcTracks  = 0;
constexpr int kSrcEngines = 8;
constexpr int kSrcLayers  = 16;
constexpr int kSrcInput   = 20;
constexpr int kSources    = 21;

// Release as a note value while Sync is on.
constexpr int         kSyncValues            = 5;
constexpr const char* kSyncNames[kSyncValues] = {"1/32", "1/16", "1/8", "1/4", "1/2"};
constexpr float       kSyncBeats[kSyncValues] = {0.125f, 0.25f, 0.5f, 1.f, 2.f};

constexpr float kHoldMaxMs    = 250.f;
constexpr float kReleaseMinMs = 10.f;
constexpr float kReleaseMaxMs = 1000.f;
constexpr float kThreshMinDb  = -48.f;

// Saved with the Performance (its "DCK1" section). Plain fields only, so
// it's written as one raw block.
struct Settings
{
    int32_t on     = 0;
    int32_t source = kSrcEngines + 6; // SMPDR
    // The notes that trigger it, one bit per MIDI note. None set = every
    // note does.
    uint8_t notes[16]  = {};
    float   hold_ms    = 10.f;
    float   release_ms = 150.f;
    int32_t sync       = 1; // Release in note values
    int32_t sync_index = 2; // 1/8
    float   thresh_db  = -18.f;
    int32_t filter     = 1; // the audio detector listens below kFilterHz
    float   amount[kChannels] = {};
};

constexpr float kFilterHz = 120.f;

inline bool IsAudioSource(int source) { return source >= kSrcLayers; }

inline bool NoteChosen(const Settings& s, int note)
{
    return note >= 0 && note < 128 && (s.notes[note >> 3] & (1u << (note & 7))) != 0;
}

inline bool AnyNoteChosen(const Settings& s)
{
    for(uint8_t b : s.notes)
        if(b)
            return true;
    return false;
}

inline void ToggleNote(Settings& s, int note)
{
    if(note >= 0 && note < 128)
        s.notes[note >> 3] ^= (uint8_t)(1u << (note & 7));
}

// Whether a note played by the source triggers the dip.
inline bool NoteDucks(const Settings& s, int note)
{
    return !AnyNoteChosen(s) || NoteChosen(s, note);
}

// The duck channel an engine (MidiSequencer::Engine) plays on.
inline int EngineChannel(int engine)
{
    static const int8_t kMap[8] = {kGr, kDxd, kDxd, kEns, kOrg, kSmv, kSmd, kSsw};
    return engine >= 0 && engine < 8 ? kMap[engine] : -1;
}

inline float ReleaseMs(const Settings& s, float bpm)
{
    if(!s.sync)
        return s.release_ms;
    const int i = s.sync_index < 0 ? 0 : (s.sync_index >= kSyncValues ? kSyncValues - 1 : s.sync_index);
    return kSyncBeats[i] * 60000.f / (bpm > 1.f ? bpm : 1.f);
}

// The dip itself: Render() fills a block with the duck depth per sample,
// 0 (none) to 1 (full). A trigger ramps it down to 1 over kAttackMs, holds,
// then lets go along a curve that comes back fast and settles gently --
// the classic pump -- reaching 0 exactly at Release. A new trigger while
// it's coming back ramps down from wherever it is.
class Envelope
{
  public:
    static constexpr float kAttackMs = 2.f;

    void Reset()
    {
        stage_ = kIdle;
        depth_ = 0.f;
    }

    // trigger_at: the sample in this block to trigger on, or -1 for none.
    void Render(float* depth, size_t n, float sr, float hold_ms, float release_ms, int trigger_at)
    {
        const float attack_step = 1.f / (kAttackMs * 0.001f * sr);
        const int   hold_n      = (int)(hold_ms * 0.001f * sr);
        const float rel_n       = release_ms * 0.001f * sr;
        for(size_t i = 0; i < n; i++)
        {
            if((int)i == trigger_at)
                stage_ = kAttack;
            switch(stage_)
            {
                case kAttack:
                    depth_ += attack_step;
                    if(depth_ >= 1.f)
                    {
                        depth_ = 1.f;
                        stage_ = kHold;
                        count_ = hold_n;
                    }
                    break;
                case kHold:
                    if(--count_ <= 0)
                    {
                        stage_ = kRelease;
                        // exp(-4 t/R) per sample, rescaled to end on 0 at R.
                        curve_ = 1.f;
                        k_     = expf(-4.f / (rel_n > 1.f ? rel_n : 1.f));
                    }
                    break;
                case kRelease:
                    curve_ *= k_;
                    depth_ = (curve_ - kE4) * (1.f / (1.f - kE4));
                    if(depth_ <= 0.f)
                    {
                        depth_ = 0.f;
                        stage_ = kIdle;
                    }
                    break;
                case kIdle: break;
            }
            depth[i] = depth_;
        }
    }

  private:
    static constexpr float kE4 = 0.01831564f; // exp(-4)
    enum Stage
    {
        kIdle,
        kAttack,
        kHold,
        kRelease
    };
    Stage stage_ = kIdle;
    float depth_ = 0.f;
    float curve_ = 0.f;
    float k_     = 1.f;
    int   count_ = 0;
};

// An audio source's kick finder: the signal (mono), through a 2-pole
// low-pass at kFilterHz when the filter's on, into a peak follower. A hit
// is the level jumping 6 dB above its lowest point while above the
// threshold. After one it waits kHoldoffMs and for the level to fall 6 dB
// below the peak it reached, so one kick is one dip, a held bass note
// triggers once, and a fast, loud pattern still re-arms between kicks.
class Detector
{
  public:
    static constexpr float kHoldoffMs = 40.f;

    void Reset()
    {
        lp1_ = lp2_ = env_ = peak_ = trough_ = 0.f;
        armed_                               = true;
        holdoff_                             = 0;
    }

    // The sample a trigger lands on in this block, or -1.
    int Process(const float* l, const float* r, size_t n, float sr, bool filter, float thresh_db)
    {
        const float a      = 1.f - expf(-6.2831853f * kFilterHz / sr);
        const float rel    = expf(-1.f / (0.05f * sr));
        const float thresh = powf(10.f, thresh_db * 0.05f);
        int         hit    = -1;
        for(size_t i = 0; i < n; i++)
        {
            float x = 0.5f * (l[i] + r[i]);
            lp1_ += a * (x - lp1_);
            lp2_ += a * (lp1_ - lp2_);
            const float v = fabsf(filter ? lp2_ : x);
            env_          = v > env_ ? v : env_ * rel;
            if(holdoff_ > 0)
                holdoff_--;
            if(armed_)
            {
                trough_ = env_ < trough_ ? env_ : trough_;
                if(holdoff_ == 0 && env_ >= thresh && env_ >= trough_ * 2.f)
                {
                    armed_   = false;
                    peak_    = env_;
                    holdoff_ = (int)(kHoldoffMs * 0.001f * sr);
                    if(hit < 0)
                        hit = (int)i;
                }
            }
            else
            {
                peak_ = env_ > peak_ ? env_ : peak_;
                if(holdoff_ == 0 && env_ < peak_ * 0.5f)
                {
                    armed_  = true;
                    trough_ = env_;
                }
            }
        }
        level_ = env_;
        return hit;
    }

    // The follower's level (0..1), for Global:Duck's meter.
    float Level() const { return level_; }

  private:
    float lp1_ = 0.f, lp2_ = 0.f, env_ = 0.f, peak_ = 0.f, trough_ = 0.f;
    bool  armed_   = true;
    int   holdoff_ = 0;
    volatile float level_ = 0.f;
};
} // namespace Duck
