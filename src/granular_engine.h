#pragma once
#include <cstdint>
#include "svf_setup.h"
#include <cstddef>
#include "daisysp.h"
#include "looper_layer.h" // FilterMode, kFilterMinHz/kFilterMaxHz

// A granular voice: one grain cloud per note, read around each voice's
// own play pointer.
// Position sets where the pointer sits, Scan moves it through the sample
// (within Loop Start/End), Spray scatters each grain's start around it,
// and Grain Size/Density set how long the grains are and how many start
// per second. Up to kNumVoices notes play at once, sharing kPoolGrains
// grains. (This replaced a monophonic two-cloud engine -- a fixed Grain
// cloud plus a separate moving Scan cloud.)
//
// Does NOT own the captured buffer -- SetSource() just points at whatever
// buffer currently holds a capture (SDRAM, owned by main.cpp), same
// convention the old engine used.
class GranularEngine
{
  public:
    void Init(float sample_rate);
    // The engine's own 16-bit stereo copy of the source (interleaved L/R,
    // capacity_frames frames, SDRAM, owned by main.cpp) -- see
    // SetSource(). Grains render from this, not from the float buffer:
    // half the SDRAM traffic per grain sample, and L/R of one frame land
    // in a single word. Must be set before the first SetSource().
    void SetSourceShadow(int16_t* buf, size_t capacity_frames)
    {
        shadow_          = buf;
        shadow_capacity_ = capacity_frames;
    }

    // len == 0 means "nothing captured yet" -- NoteOn() is a no-op then.
    // Silences both grain clusters outright (a grain's position was
    // computed against the OLD src_len_, and a shorter new capture could
    // otherwise leave it out of bounds) -- same reasoning the old engine's
    // SetSource() used.
    void SetSource(const float* buf_l, const float* buf_r, size_t len);
    // Live re-trim of the SAME already-loaded capture (Screen::Granular's
    // Trim page) -- updates the readable range like SetSource() does,
    // but deliberately skips its hard reset (clearing both grain
    // clusters, cancelling the held note) AND its waveform-peaks
    // recompute. The reset isn't actually needed for safety here: every
    // grain read already clamps to src_len_ independently on every
    // sample (see RenderGrain()), so a currently-playing note simply
    // keeps sounding smoothly through a live trim adjustment instead of
    // being cut on every single knob tick. The peaks recompute is left
    // to the caller (see RecomputeWaveformPeaks() below) specifically so
    // it can be throttled to a real display-refresh rate instead of
    // running full-speed on every control-loop tick -- it's an O(full
    // capture length) scan (up to ~480,000 samples), genuinely too
    // costly to redo at knob-polling rate; doing so was a second real,
    // user-reported cause of "still jumpy, slow to update" on this page
    // even after the reset fix above.
    // Also O(1) on the 16-bit copy when the trimmed range lies inside the
    // buffer last given to SetSource() (always the case for Trim) -- it
    // just offsets into the existing copy instead of rebuilding it.
    void SetTrimRange(const float* buf_l, const float* buf_r, size_t len);
    // Rebuilds waveform_peaks_ from whatever src_l_/src_len_ currently
    // point at -- called automatically by SetSource() (a rare, one-time
    // event, fine to always do immediately), and separately, throttled,
    // by Trim's own live-adjustment path (see SetTrimRange()'s comment
    // for why that one can't just call this every tick too).
    void RecomputeWaveformPeaks();
    // Straight, honest linear playback of src_l_/src_r_/src_len_ (i.e.
    // exactly whatever Trim currently has selected, since SetTrimRange()
    // already points those at it -- added after a direct request for a
    // way to actually hear the trimmed section rather than inferring it
    // from the waveform/grains alone) -- mixed in alongside Grain/Scan
    // in Process(), through the same filter/pan/output-level chain, not
    // a separate bypassed preview path, so what you hear here matches
    // what playing a note will actually sound like. One-shot: (re)starts
    // from the beginning every call, auto-stops on its own once
    // preview_pos_ reaches src_len_ -- see Process()'s own comment.
    void PlayTrimPreview();
    bool   HasSource() const { return src_len_ > 0; }
    size_t GetSourceLen() const { return src_len_; }

    // Up to kNumVoices notes at once (Poly, the default) sharing one pool
    // of kPoolGrains grains -- one note gets the whole pool, a chord
    // splits it. Mono plays one voice, last note wins. A 5th note steals
    // the oldest voice, its grains fading out instead of cutting.
    void NoteOn(uint8_t note, uint8_t velocity);
    void NoteOff(uint8_t note);
    // MIDI CC 123 (All Notes Off)/120 (All Sound Off) -- some controllers
    // don't reliably send a matching NoteOff, leaving a note stuck.
    void AllNotesOff();
    bool IsNoteActive() const;
    void SetPoly(bool on);
    bool GetPoly() const { return poly_; }

    // --- Grain Size / Density (GR-1 names). Density is grains per
    // second per voice, 0.5 to 500 Hz on an exponential knob. ----------
    void  SetSize01(float v01);
    float GetSize01() const { return size01_; }
    float GetSizeMs() const { return size01_ * (kMaxGrainMs - kMinGrainMs) + kMinGrainMs; }
    void  SetDensity01(float v01);
    float GetDensity01() const { return density01_; }
    float GetDensityHz() const { return density_hz_; }

    // --- Rhythm -- gates WHICH of a voice's already-scheduled hops
    // actually spawn a grain, rather than changing the timing itself --
    // a skipped hop is a real silence, which is what makes the gaps
    // genuinely uneven. Off (every hop fires), a small named bank of
    // grooves, then two Euclidean stops. Each voice's step clock resets
    // to 0 on its NoteOn so a pattern always starts the same way.
    void        CycleRhythm();
    int         GetRhythmIndex() const { return rhythm_index_; }
    const char* GetRhythmName() const { return rhythm_name_cached_; }
    uint16_t    GetRhythmMask() const { return rhythm_mask_cached_; }
    static constexpr int kRhythmSteps = 16;

    // --- Speed -- how often a Rhythm pattern's steps happen, decoupled
    // from Density. No effect at Rhythm Off.
    enum class GrainSpeed
    {
        Slow,
        Medium,
        Fast,
        Sync // locks to the Looper's live tempo, see SetExternalBpm()
    };
    void        CycleGrainSpeed();
    GrainSpeed  GetGrainSpeed() const { return grain_speed_; }
    const char* GetGrainSpeedName() const;
    // Pushed in live from Ui (which owns the TempoClock) once per
    // main-loop tick -- see ComputeRhythmStepSamples()'s Sync case.
    void SetExternalBpm(float bpm) { external_bpm_ = bpm; }

    // --- Position / Spray (GR-1). Position is where every voice's play
    // pointer sits, live: Scan moves each voice away from it, and turning
    // Position carries the whole cloud with it. Spray scatters each
    // grain's start up to +/- SprayMs around the pointer (0..2 s). ------
    void  SetPosition01(float v01);
    float GetPosition01() const { return position01_; }
    void  SetSpray01(float v01);
    float GetSpray01() const { return spray01_; }
    float GetSprayMs() const { return spray01_ * spray01_ * kMaxSprayMs; }

    // --- Scan (GR-1): how fast each voice's pointer moves through the
    // sample, -2x (reverse) to +2x the sample's own speed, with a dead
    // zone at the centre where the pointer holds still. 1x with Pitch at
    // 0 replays the sample at its own speed; slower stretches it in time
    // without changing pitch. Every note starts its pointer at Position.
    void  SetScan01(float v01);
    float GetScan01() const { return scan01_; }
    float GetScanSpeed() const { return scan_speed_; } // signed, in x
    // What the pointer does at the Loop edges.
    enum class ScanMode
    {
        Loop,     // wrap to the other edge
        OneShot,  // stop at the edge
        PingPong  // reverse at each edge
    };
    void        CycleScanMode() { scan_mode_ = (ScanMode)(((int)scan_mode_ + 1) % 3); }
    ScanMode    GetScanMode() const { return scan_mode_; }
    const char* GetScanModeName() const;

    // --- Loop Start / End (GR-1): the area the pointer scans within.
    // Default 0..1 is the whole sample. Start above End just works.
    void  SetLoopStart01(float v01);
    float GetLoopStart01() const { return loop_start01_; }
    void  SetLoopEnd01(float v01);
    float GetLoopEnd01() const { return loop_end01_; }

    // --- Pitch -- a fixed per-grain offset (+/-24 semitones), plus
    // keyboard tracking (each note's own pitch, 12-TET against middle C).
    void  SetGrainTuneSemitones01(float v01); // quantized to whole semitones, +-24
    float GetGrainTuneSemitones01() const;
    int   GetGrainTuneSemitones() const { return grain_tune_semitones_; }
    void  SetGrainFollowsNote(bool on) { grain_follows_note_ = on; }
    bool  GetGrainFollowsNote() const { return grain_follows_note_; }

    // --- Direction -- which way each grain plays its own slice,
    // independent of which way Scan moves the pointer (scan forward with
    // every grain reversed, say).
    enum class Direction
    {
        Forward,
        Reverse,
        Random
    };
    void        SetDirection01(float v01); // quantized to the 3 states above
    float       GetDirection01() const;
    void        CycleDirection() { direction_ = (Direction)(((int)direction_ + 1) % 3); }
    Direction   GetDirection() const { return direction_; }
    const char* GetDirectionName() const;

    // --- Note-level ADSR -- shapes the whole voice's loudness across a
    // held note, layered ON TOP of (not instead of) the fixed per-grain
    // Hann window below (that only prevents clicks within a grain, it
    // has no swell-in/tail-off of its own). Same curved-seconds
    // convention as PadSynth's own ADSR. -------------------------------
    void  SetAttack01(float v01);
    float GetAttack01() const { return attack01_; }
    float GetAttackSeconds() const;
    void  SetDecay01(float v01);
    float GetDecay01() const { return decay01_; }
    float GetDecaySeconds() const;
    void  SetSustain01(float v01);
    float GetSustain01() const { return sustain01_; }
    void  SetRelease01(float v01);
    float GetRelease01() const { return release01_; }
    float GetReleaseSeconds() const;

    // --- Bus filter (post-mix, same shape as PadSynth's own) ----------
    void       SetFilterMode(FilterMode m) { filter_mode_ = m; }
    FilterMode GetFilterMode() const { return filter_mode_; }
    void       SetFilterCutoff01(float v01) { filter_cutoff01_ = v01; }
    float      GetFilterCutoff01() const { return filter_cutoff01_; }
    void       SetFilterResonance01(float v01) { filter_res01_ = v01; }
    float      GetFilterResonance01() const { return filter_res01_; }

    // --- Volume -- the cloud's own level into the chain, kept from the
    // two-cloud engine (its Grain volume) so older presets keep their
    // loudness. Not on a page any more: Mix has Level/Pan/Gain like
    // every other engine.
    void  SetGrainVolume01(float v01) { grain_volume01_ = v01; }
    float GetGrainVolume01() const { return grain_volume01_; }

    // --- Overall output level -- applied AFTER the Grain/Scan mix above,
    // a separate control from it (this is "how loud is Grains in the
    // master mix", not "how loud is Scan relative to Grain"). Needed for
    // the Global Mixer page, which controls Pad's and Grains' levels
    // side by side the same way PadSynth::SetOutputLevel01() already
    // does for Pad. -----------------------------------------------------
    void  SetOutputLevel01(float v01);
    float GetOutputLevel01() const { return output_level01_; }
    // Gain into the soft limiter, 0 to +12 dB (1x to 4x) -- same control
    // as EnsembleSynth::SetGain01(). Session-level like Output Level and
    // Pan: not part of GranularPresetData.
    void  SetGain01(float v01)
    {
        gain01_ = v01 < 0.f ? 0.f : (v01 > 1.f ? 1.f : v01);
        gain_   = powf(4.f, gain01_);
    }
    float GetGain01() const { return gain01_; }

    // True for a hold period after the PRE-limiter signal (before
    // tanhf(), see Process()) last crossed unity -- "the limiter is
    // actually compressing hard right now," not just "the output is
    // loud." Same reasoning as DexedSynth::IsLimiting(): tanhf()
    // mathematically bounds its own output to (-1,1) regardless of how
    // hard the pre-limiter signal is driving it, so a clip check on the
    // result could never usefully fire -- this engine's own
    // output_level_ in particular can reach up to 12x (see
    // SetOutputLevel01()), so the gap between "driving the limiter
    // hard" and "the bounded output looks loud" is even bigger here
    // than for Dexed.
    bool IsLimiting() const { return pre_limiter_clip_hold_ > 0.f; }

    // Live level for Screen::Mixer's own Peak bar -- same PRE-limiter
    // tap point as IsLimiting() above, not the post-chain output this
    // engine hands back to main.cpp. That signal looked permanently
    // quiet even when genuinely driving the limiter hard: it's post-
    // tanhf() (bounded to (-1,1) regardless of input), post-output_level_
    // (which can reach up to 12x but is often well under unity), AND
    // post-pan (halved again at dead center). This reads before any of
    // that, so a genuinely hot signal now visibly reaches the top of
    // the bar.
    float GetPreLimiterLevel01() const { return pre_limiter_level_; }

    // --- Reverb send -- was previously an unconditional full send (no
    // control at all); now a real continuous knob, reachable from both
    // this engine's own Mix page and Global:Mixer -- same underlying
    // value either way (this field), so changing it in one place updates
    // the other with no extra sync needed, same as PadSynth's own
    // reverb send. Saved in GranularPresetData (see its own field's
    // comment) -- the one exception to the "session-level, not saved"
    // treatment Delay Send/Pan/Output Level below still get, added after
    // a direct request to match how DexedPresetData already saves its
    // own reverb_send01.
    void  SetReverbSend01(float v01) { reverb_send01_ = v01; }
    float GetReverbSend01() const { return reverb_send01_; }

    // Own independent send into the shared delay bus (main.cpp's
    // fx_delay_l/r) -- same relationship to it Reverb Send above has to
    // fx_reverb_shared, added on Grains' own new FX page. Same session-
    // level (not saved in presets) treatment Reverb Send used to have,
    // before that one became the one exception -- see its own comment.
    void  SetDelaySend01(float v01) { delay_send01_ = v01; }
    float GetDelaySend01() const { return delay_send01_; }

    // --- Pan -- same linear law as LooperLayer/PadSynth (panL = 1-v,
    // panR = v), applied to the already-stereo captured signal (Grains
    // reads real L/R from whatever was captured/imported, unlike Pad's
    // mono voice sum) right before writing out_l/out_r. A session-level
    // mixer setting, not part of the captured sound's own identity --
    // same reasoning that still excludes output_level01_/delay_send01_
    // from GranularPresetData below (reverb_send01_ used to be in this
    // group too -- see its own comment for why it no longer is).
    void  SetPan01(float v01);
    float GetPan01() const { return pan01_; }

    // Renders `size` samples, WRITES into out_l/out_r (see PadSynth's own
    // Process() doc comment for why -- main.cpp needs this exact signal
    // for more than one consumer). ADDS into reverb_send_l/r (already
    // zeroed by the caller for this block, same convention every other
    // reverb-send contributor in main.cpp uses).
    void Process(size_t      size,
                 float*      out_l,
                 float*      out_r,
                 float*      reverb_send_l,
                 float*      reverb_send_r,
                 float*      delay_send_l,
                 float*      delay_send_r);

    // --- Visuals ---------------------------------------------------
    // Each sounding voice's live pointer (0..1 across the source), up to
    // max_out of them. Returns how many were written.
    int   GetVoicePointers01(float* out, int max_out) const;
    // What fraction of the whole buffer one grain spans at the current
    // Size -- lets the waveform display draw Position as a real span.
    float GetGrainSizeFraction01() const;
    // Same "cached once in SetSource(), cheap to redraw from" convention
    // as LooperLayer::GetWaveformPeaks() -- the whole-sample display
    // needs the FULL buffer's peaks, unlike the old engine's zoomed-window
    // view which only ever needed a small slice.
    static constexpr int kWaveformCols = 63;
    const float* GetWaveformPeaks() const { return waveform_peaks_; }

    // Read-only access to whatever SetSource() currently points at --
    // needed so a preset save can write the actual captured audio out
    // (this engine doesn't own that buffer, see SetSource()'s own doc
    // comment, so it can't save it itself).
    const float* GetSourceL() const { return src_l_; }
    const float* GetSourceR() const { return src_r_; }

    // Flat snapshot of every knob-adjustable setting above (NOT the
    // captured audio itself -- that's saved/loaded separately, see
    // PerformanceStore::SaveGranularPreset()/LoadGranularPreset()), same
    // "one place that knows how to capture/restore the whole instrument's
    // state" idiom as PadSynth::PadPresetData.
    struct GranularPresetData
    {
        float   size01            = 0.4f;
        float   fill01            = 0.667f; // matches the class's own default fill_count_=2
        float   scan_fill01       = 0.667f; // matches the class's own default scan_fill_count_=2
        float   gap01             = 0.1f;
        float   scan_gap01        = 0.1f; // matches the class's own default scan_gap01_
        float   position01        = 0.f;
        float   scan01            = 0.5f; // Scan speed/direction, center = off
        float   scan_start01      = 0.f;  // Scan range Start, buffer start
        float   scan_end01        = 1.f;  // Scan range End, buffer end
        float   grain_tune01      = 0.5f; // 0 semitones
        bool    grain_follows_note = false;
        float   direction01       = 0.1667f; // Forward
        int32_t scan_direction    = (int32_t)Direction::Forward;
        float   attack01          = 0.3f;
        float   decay01           = 0.3f;
        float   sustain01         = 0.8f;
        float   release01         = 0.4f;
        int32_t filter_mode       = (int32_t)FilterMode::Off;
        float   filter_cutoff01   = 1.f;
        float   filter_res01      = 0.f;
        float   grain_volume01    = 0.60f;
        float   scan_volume01     = 0.80f;
        int32_t rhythm_index      = 0; // 0 = Off, see CycleRhythm()
        int32_t grain_speed       = (int32_t)GrainSpeed::Slow;
        float   jitter01          = 0.f; // 0 = off, see SetJitter01()
        bool    scan_bounce       = true; // matches the class's own default
        // Trailing field, added after Delay Send/Pan/Output Level had
        // already established "session-level, not saved" for this whole
        // group -- see SetReverbSend01()'s own comment for why this one
        // specifically was pulled back out of that group, to match
        // DexedPresetData's own reverb_send01. Default matches the live
        // class member's own default (0.f) rather than picking a new
        // number -- unlike Dexed's own 0.6 output_level01 default,
        // there's no factory-preset-defaulting concern here to justify a
        // different value. Any GranularPresetData written before this
        // field existed is handled by a clean file-version reject (see
        // kGranularPresetFileVersion in performance_store.cpp), not a
        // silent default fill-in. Briefly suspected (and directly
        // ruled out by reverting it alone and re-testing) as the cause
        // of a real save failure found elsewhere -- see
        // kGranularPresetFileVersion's own comment for the real cause.
        float   reverb_send01     = 0.f;
        // Added in preset file v11, when the Mix page's Level/Pan/Gain
        // and Delay send joined the preset like every other engine's.
        // Negative = not stored (v10 files and "New"): loading leaves
        // the current value alone, so old presets sound as they did.
        float   output_level01    = -1.f;
        float   pan01             = -1.f;
        float   gain01            = -1.f;
        float   delay_send01      = -1.f;
        // The Ui's own Grains MIDI channel (-1 = Omni), filled in by the
        // Ui on a user save -- CapturePreset() never sets it. -2 = not
        // stored: loading leaves the current channel alone.
        int32_t midi_channel      = -2;
        // Added in preset file v12 with the one-cloud, 4-voice engine.
        // Fields above that it no longer uses (fill/gap, the Scan layer's
        // fill/gap/volume/direction, jitter, scan01's old meaning) stay so
        // older files still load: density01 < 0 means "older preset", and
        // ApplyPreset() converts them as closely as one cloud can.
        float   density01         = -1.f;
        float   spray01           = -1.f;
        float   scan_speed01      = -1.f; // the new Scan knob; scan01 above is the old one
        int32_t scan_mode         = -1;
        int32_t poly              = -1;   // -1 = not stored: Poly
    };
    void               ApplyPreset(const GranularPresetData& p);
    GranularPresetData CapturePreset() const;

  private:
    static constexpr int kNumVoices  = 4;
    static constexpr int kPoolGrains = 32;

    struct Grain
    {
        bool    active    = false;
        uint8_t voice     = 0;   // whose envelope this grain is heard through
        float   phase     = 0.f; // 0..1 through the grain's own Hann window
        float   phase_inc = 0.f;
        float   read_pos  = 0.f; // absolute sample index into the source
        float   read_inc  = 1.f; // signed -- direction + pitch combined
        float   gain      = 1.f;
    };

    struct Voice
    {
        int      note         = -1;    // the key this voice plays (kept through release)
        bool     gate         = false; // key held
        uint32_t triggered_at = 0;
        float    note_rate    = 1.f;   // 2^((note-60)/12)
        float    note_gain    = 1.f;   // sqrt(velocity/127)
        float    offset       = 0.f;   // pointer distance from Position, in samples
        float    scan_sign    = 1.f;   // PingPong's current heading
        float    countdown    = 0.f;   // samples until the next grain
        int      rhythm_step  = 0;
        daisysp::Adsr adsr;
    };

    // Steal a pool slot for voice v and start a grain there -- see the
    // .cpp for how a chord shares the pool.
    void  TriggerGrain(int v, float read_pos, float read_inc, float gain);
    // Renders one grain for `count` consecutive samples, ADDING into
    // out_l/out_r. fade/fade_inc: the release-fade gain and its per-sample
    // decrement (nullptr/0 for a live grain).
    void  RenderGrainSpan(Grain& g, float* fade, float fade_inc, float* out_l, float* out_r,
                           size_t count) const;
    // Every pool grain (live and fading) from chunk sample `from` to
    // `to`, each into its own voice's buffer. Process() calls this up to
    // each trigger point so slot stealing sees exact grain phases.
    void  RenderPoolSpan(size_t from, size_t to);
    // Fades out every grain voice v owns (a stolen voice, or Mono's
    // new note) instead of cutting them.
    void  ChokeVoice(int v);
    bool  VoiceBusy(int v) const;
    float PointerSamples(const Voice& vc) const;
    void  AdvancePointer(Voice& vc, float lo, float hi);
    float GrainLenSamples() const;
    float HopSamples() const; // one voice's grain interval at Rhythm Off
    float CurrentGrainIntervalSamples() const;
    float ComputeRhythmStepSamples() const;
    float ReadHann(float phase01) const;
    float ResolveDirectionSign();
    float Random01();
    void  RecomputeRhythmPattern();
    // An older preset's Scan speed was a fraction of the buffer per
    // second; in x it depends on the buffer's length, so it waits here
    // until a source is loaded.
    void  ResolveLegacyScan();
    // Fills shadow_ from buf_l/buf_r (len frames), peak-normalized to the
    // full int16 range, and sets src_inv_scale_ to undo that at render.
    void  BuildShadow(const float* buf_l, const float* buf_r, size_t len);

    // src_l_/src_r_ stay float for the trim preview and waveform peaks;
    // src_s16_ (same range, into shadow_) is what grains actually read.
    const float*   src_l_   = nullptr;
    const float*   src_r_   = nullptr;
    const int16_t* src_s16_ = nullptr;
    size_t         src_len_ = 0;
    float          src_inv_scale_ = 1.f;
    int16_t*       shadow_          = nullptr;
    size_t         shadow_capacity_ = 0;
    const float*   shadow_base_l_   = nullptr; // float buffer shadow_ was built from
    size_t         shadow_base_len_ = 0;
    float        sample_rate_ = 48000.f;

    // See PlayTrimPreview()'s own comment.
    bool   preview_active_ = false;
    size_t preview_pos_    = 0;

    Voice    voices_[kNumVoices];
    bool     poly_        = true;
    uint32_t trigger_seq_ = 0;

    // Live grains, and a fade-out slot per grain for one that's stolen
    // or choked mid-window (an instant cut clicks).
    Grain grains_[kPoolGrains];
    Grain release_grains_[kPoolGrains];
    float release_fades_[kPoolGrains] = {};

    float size01_     = 0.4f;
    float density01_  = 0.5f;
    float density_hz_ = 15.8f; // cached by SetDensity01()
    static constexpr float kMinGrainMs     = 1.f;
    static constexpr float kMaxGrainMs     = 1000.f;
    static constexpr float kMinDensityHz   = 0.5f;
    static constexpr float kMaxDensityHz   = 500.f;
    static constexpr float kMaxSprayMs     = 2000.f;
    static constexpr float kMaxScanSpeed   = 2.f;
    static constexpr float kScanDeadZone   = 0.04f;

    // --- Rhythm pattern gate + Speed ----------------------------------
    static constexpr int kNumRhythmStates = 7; // Off + 4Floor/Tresillo/OffBeat/Sparse + 2 Euclid
    int         rhythm_index_        = 0;      // 0 = Off
    uint16_t    rhythm_mask_cached_  = 0xFFFF; // Off = every bit set = every hop fires
    const char* rhythm_name_cached_  = "Off";
    GrainSpeed  grain_speed_   = GrainSpeed::Slow;
    float       external_bpm_ = 120.f;

    float position01_ = 0.f;
    float spray01_    = 0.f;

    float    scan01_       = 0.5f; // centre = pointer holds still
    float    scan_speed_   = 0.f;  // signed, samples per sample
    ScanMode scan_mode_    = ScanMode::Loop;
    float    loop_start01_ = 0.f;
    float    loop_end01_   = 1.f;
    bool     legacy_scan_pending_ = false;
    float    legacy_scan_frac_    = 0.f; // signed fraction of the buffer per second

    int   grain_tune_semitones_ = 0;
    float grain_tune_rate_      = 1.f; // 2^(semitones/12)
    bool  grain_follows_note_   = true;

    Direction direction_ = Direction::Forward;
    uint32_t  rng_state_ = 0x9E3779B9u; // xorshift32 seed

    float attack01_ = 0.3f, decay01_ = 0.3f, sustain01_ = 0.8f, release01_ = 0.4f;
    static constexpr float kMinAdsrSeconds = 0.005f;
    static constexpr float kMaxAdsrSeconds = 3.f;

    TptSvf filter_l_, filter_r_;
    FilterMode   filter_mode_     = FilterMode::Off;
    float        filter_cutoff01_ = 1.f;
    float        filter_res01_    = 0.f;

    float grain_volume01_  = 0.60f;
    float output_level01_  = 0.70f;
    float output_level_    = 1.f; // powf(output_level01_, 2.5f)*12, cached by the setter
    float gain01_          = 0.f;
    float gain_            = 1.f; // powf(4, gain01_), cached by SetGain01()
    // Chord headroom: 1/sqrt(voices sounding), ramped per sample so a
    // note starting or ending never steps the level.
    float chord_scale_     = 1.f;
    // See IsLimiting()'s own comment. Checked against the PRE-tanhf()
    // value in Process().
    float pre_limiter_clip_hold_ = 0.f;
    // Loudest output sample of the last block -- Process() only skips while
    // this is silent, so a filter's ringing tail is never cut off.
    float last_block_peak_ = 0.f;
    float pre_limiter_level_     = 0.f; // see GetPreLimiterLevel01()
    float reverb_send01_   = 0.f;
    float delay_send01_    = 0.f;

    float pan01_      = 0.5f;
    float pan_l_gain_ = 0.7071f; // EqualPowerPan(pan01_), control-rate cache
    float pan_r_gain_ = 0.7071f;

    // Read nearest-entry (no interpolation) -- 1024 points keeps the
    // stepping error ~-56dB below each grain, masked by its own content.
    static constexpr int kHannTableSize = 1024;
    float hann_table_[kHannTableSize];

    // Per-chunk scratch for Process(): a control pass (envelopes,
    // pointers, triggers) renders grains into each voice's buffer up to
    // every trigger point, then a mix pass applies each voice's envelope.
    static constexpr size_t kRenderChunk = 64;
    float chunk_l_[kNumVoices][kRenderChunk];
    float chunk_r_[kNumVoices][kRenderChunk];
    float chunk_env_[kNumVoices][kRenderChunk];

    float waveform_peaks_[kWaveformCols] = {};
};
