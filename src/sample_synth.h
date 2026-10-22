#pragma once
#include <cstdint>
#include <cstddef>
#include "daisysp.h"
#include "looper_layer.h" // FilterMode, kFilterMinHz/kFilterMaxHz

// A 10-voice polyphonic sample player -- the third synth engine
// alongside DexedSynth (FM) and GranularEngine (granular texture).
// Combines two proven patterns already in this codebase rather than
// inventing new ones: DexedSynth's own Voice[]/FindVoiceForNote()
// oldest-note-steal allocator (dexed_synth.h/.cpp), and
// GranularEngine's pitch-tracking math (note_rate_ = 2^((note-60)/12))
// plus its clamp-at-ends linear-interpolated buffer read (RenderGrain()).
//
// A SampleSlot is "one loaded sample plus how it maps to notes" --
// deliberately separate from a Voice (which is "one currently-sounding
// playback instance"). Multiple voices can read the same slot's buffer
// at once (e.g. a chromatic chord) since playback never writes to it.
// Slot audio itself is NOT owned by SampleSynth -- same "does not own
// the buffer" convention GranularEngine::SetSource() already uses --
// a caller (PerformanceStore's kit loader) owns the SDRAM-backed sample
// storage and hands in pointers via LoadSlot().
//
// Explicitly does NOT have the Layer effects cycle (Drive/Bitcrush/
// Chorus/etc.) or the Dexed-only wavefolder -- just filter, per-voice
// ADSR, and the two shared reverb/delay sends, per direct request.
class SampleSynth
{
  public:
    void Init(float sample_rate);

    static constexpr int kMaxVoices = 10;
    // 24, not some rounder number -- matches the real OP-1/OP-XY drum
    // engine's own 24-slot layout exactly, so an authentic multi-sample
    // kit (24 marker-sliced hits) never has to drop any of its slices.
    static constexpr int kMaxSampleSlots = 24;

    // Points slot `index` at already-loaded sample audio (SDRAM-backed,
    // owned by the caller -- see this class's own doc comment). len==0
    // clears the slot (NoteOn() then never matches it). chromatic=false
    // means "fixed native pitch, ignore the incoming note's pitch" (the
    // drum/one-shot case); chromatic=true pitch-shifts across
    // [low_note,high_note] around root_note, same 2^((note-root)/12)
    // formula GranularEngine::NoteOn() already uses.
    void ClearSlot(int index);
    void LoadSlot(int index, const float* buf_l, const float* buf_r, int32_t len,
                   bool chromatic, uint8_t root_note, uint8_t low_note, uint8_t high_note,
                   float volume01);
    bool  SlotHasSample(int index) const { return index >= 0 && index < kMaxSampleSlots && slots_[index].len > 0; }

    // Capture/Trim's own "Play" button (Ui::SampleSynthParamPage::Trim)
    // -- a plain, un-pitched, un-gated one-shot readout of slot 0 ONLY,
    // straight through at native speed regardless of that slot's own
    // chromatic/root_note (Tune is for keyboard playback, not for
    // judging the raw trimmed content itself) -- same "always restart
    // from the beginning, tap to retrigger" convention and reasoning as
    // GranularEngine::PlayTrimPreview(). Does not touch voices_[]/ADSR
    // at all -- mixed directly into Process()'s own sum.
    void PlayPreview();

    // ClearSlot()/LoadSlot() only ever touch slots_[] -- a voice
    // currently sounding (held_note != -1, or mid-release-tail with
    // adsr.IsRunning() still true) keeps referencing its own slot index
    // by number, not by content, so swapping that slot's buffer out
    // from under it makes it immediately jump to playing the NEW audio
    // with no NoteOn at all -- heard as "it just starts playing by
    // itself" right after a Kit/Sound load. Callers that replace many
    // slots at once (a whole Kit, or clearing all 24 for a single
    // Sound) call this first so no stale voice survives the swap.
    void ResetAllVoices();

    // Same 3-tier allocation as DexedSynth::FindVoiceForNote() --
    // retrigger the same note if already sounding, else prefer a free
    // AND genuinely idle voice (adsr.IsRunning()==false, the direct
    // equivalent of DexedSynth::VoiceIsIdle()) over one merely freed-
    // but-still-tailing, else steal the oldest by trigger order. A note
    // that matches no slot's range is a no-op -- no voice is touched.
    void NoteOn(uint8_t note, uint8_t velocity);
    // Frees held_note immediately (same convention as DexedSynth::
    // NoteOff()) -- the voice's own Adsr release tail keeps rendering
    // regardless, same as every other engine here. Calls
    // Adsr::ForceRelease() (not just clearing held_note) -- the real
    // fix for a confirmed-on-hardware stuck note found in EnsembleSynth
    // this engine shares the identical risk for: a fast NoteOn
    // immediately followed by this NoteOff, with no audio block ever
    // processed with gate=true in between, leaves Adsr's own gate-edge
    // detection never seeing the release. See Adsr::ForceRelease()'s own
    // doc comment for the full mechanism.
    void NoteOff(uint8_t note);
    // MIDI CC 123 (All Notes Off)/120 (All Sound Off) -- same real-world
    // need as DexedSynth::AllNotesOff() (see its own comment, including
    // the confirmed-on-hardware Akai MPK Mini Plus arp repro): releases
    // every currently-held voice regardless of note number.
    void AllNotesOff();

    // --- Pitch bend + mod wheel -- MIDI-controlled only, no UI page
    // (same "no own page, just a real-time controller input" treatment
    // DexedSynth::SetPitchBend14bit()/SetModWheel01() already get).
    // Bend range is +-2 semitones, same as Dexed's own default pitch
    // bend range (DexedSynth's kControllerPitchRange). Mod wheel drives
    // a simple pitch vibrato (classic "mod wheel = vibrato" convention)
    // since this engine has no filter-cutoff-mod or LFO concept of its
    // own to modulate instead.
    void SetPitchBend14bit(uint16_t raw14); // 0..16383, 8192 = center
    void SetModWheel01(float v01);

    // --- Per-voice ADSR -- one knob set shapes every voice identically,
    // same "one knob configures N internal objects" idiom
    // GranularEngine's own Attack/Decay/Sustain/Release already uses. --
    void  SetAttack01(float v01);
    float GetAttack01() const { return attack01_; }
    float GetAttackSeconds() const
    {
        return kMinAdsrSeconds * powf(kMaxAdsrSeconds / kMinAdsrSeconds, attack01_);
    }
    void  SetDecay01(float v01);
    float GetDecay01() const { return decay01_; }
    float GetDecaySeconds() const
    {
        return kMinAdsrSeconds * powf(kMaxAdsrSeconds / kMinAdsrSeconds, decay01_);
    }
    void  SetSustain01(float v01);
    float GetSustain01() const { return sustain01_; }
    void  SetRelease01(float v01);
    float GetRelease01() const { return release01_; }
    float GetReleaseSeconds() const
    {
        return kMinAdsrSeconds * powf(kMaxAdsrSeconds / kMinAdsrSeconds, release01_);
    }

    // --- Bus filter (post-mix) -- verbatim same shape as DexedSynth/
    // GranularEngine's own (FilterMode enum shared via looper_layer.h). -
    void       SetFilterMode(FilterMode m) { filter_mode_ = m; }
    FilterMode GetFilterMode() const { return filter_mode_; }
    void       SetFilterCutoff01(float v01) { filter_cutoff01_ = v01; }
    float      GetFilterCutoff01() const { return filter_cutoff01_; }
    void       SetFilterResonance01(float v01) { filter_res01_ = v01; }
    float      GetFilterResonance01() const { return filter_res01_; }

    // --- Output level / Pan -- same curve/law every other engine here
    // uses (SetOutputLevel01: powf(v01,2.5f)*1.4f; Pan: linear panL=1-v,
    // panR=v). ------------------------------------------------------
    void  SetOutputLevel01(float v01);
    float GetOutputLevel01() const { return output_level01_; }
    void  SetPan01(float v01);
    float GetPan01() const { return pan01_; }

    // --- Reverb/delay send -- identical shape to DexedSynth/
    // GranularEngine's own, same shared buses in main.cpp. -----------
    void  SetReverbSend01(float v01) { reverb_send01_ = v01; }
    float GetReverbSend01() const { return reverb_send01_; }
    void  SetDelaySend01(float v01) { delay_send01_ = v01; }
    float GetDelaySend01() const { return delay_send01_; }

    // Same PRE-limiter tap convention as DexedSynth::IsLimiting()/
    // GetPreLimiterLevel01() -- see those for the full reasoning (a
    // post-tanhf()/post-output_level_/post-pan check could never
    // usefully fire).
    bool  IsLimiting() const { return pre_limiter_clip_hold_ > 0.f; }
    float GetPreLimiterLevel01() const { return pre_limiter_level_; }

    // Renders `size` samples. WRITES out_l/out_r (main.cpp needs this
    // exact signal for more than one consumer, same reasoning as every
    // other engine's own Process() doc comment). ADDS into
    // reverb_send_l/r and delay_send_l/r (already zeroed by the caller
    // for this block, same shared-bus convention every other
    // contributor uses).
    void Process(size_t size, float* out_l, float* out_r, float* reverb_send_l,
                 float* reverb_send_r, float* delay_send_l, float* delay_send_r);

    // Flat snapshot of every knob-adjustable setting -- NOT the sample
    // audio itself (that's identified by filename/slot assignment, see
    // PerformanceStore's own kit save/load), same "preset != captured
    // audio" split GranularPresetData already uses.
    struct SampleSynthPresetData
    {
        float   attack01        = 0.01f;
        float   decay01         = 0.3f;
        float   sustain01       = 0.8f;
        float   release01       = 0.2f;
        int32_t filter_mode     = (int32_t)FilterMode::Off;
        float   filter_cutoff01 = 1.f;
        float   filter_res01    = 0.f;
        float   output_level01  = 0.8f;
        float   pan01           = 0.5f;
        float   reverb_send01   = 0.f;
        float   delay_send01    = 0.f;
    };
    void                   ApplyPreset(const SampleSynthPresetData& p);
    SampleSynthPresetData  CapturePreset() const;

  private:
    struct SampleSlot
    {
        const float* buf_l = nullptr;
        const float* buf_r = nullptr;
        int32_t      len   = 0;
        bool         chromatic = false;
        uint8_t      root_note = 60;
        uint8_t      low_note  = 0;
        uint8_t      high_note = 127;
        float        volume01  = 0.8f;
    };
    SampleSlot slots_[kMaxSampleSlots];

    struct Voice
    {
        int           held_note   = -1; // -1 = free
        uint8_t       velocity     = 100;
        uint32_t      triggered_at = 0;
        int           slot         = -1; // which SampleSlot this voice reads from
        float         read_pos     = 0.f;
        float         read_inc     = 1.f;
        daisysp::Adsr adsr;
    };
    Voice    voices_[kMaxVoices];
    uint32_t trigger_seq_ = 0;

    int FindVoiceForNote(uint8_t note);
    // Renders one voice's own next sample (clamp-at-ends linear
    // interpolation, same shape/reasoning as GranularEngine::
    // RenderGrain() -- a one-shot sample is not a seamless loop, so
    // wrapping at the ends would fabricate a real discontinuity).
    void RenderVoice(Voice& v, float& out_l, float& out_r);

    float sample_rate_ = 48000.f;

    // Pitch bend + mod wheel (vibrato) -- see SetPitchBend14bit()/
    // SetModWheel01()'s own comments. current_pitch_mult_ is recomputed
    // once per sample in Process() (bend * vibrato LFO) and read by
    // RenderVoice() for every active voice that same sample -- a global
    // multiplier on top of each voice's own read_inc, not baked into
    // read_inc itself, since bend/vibrato both change continuously
    // while a note is still held.
    static constexpr float kPitchBendSemitones  = 2.f; // same range Dexed's own default uses
    static constexpr float kVibratoHz           = 5.f;
    static constexpr float kVibratoMaxSemitones = 0.5f; // depth at full mod wheel
    float pitch_bend01_      = 0.f; // -1..1, 0 = center
    float pitch_bend_rate_   = 1.f; // 2^(bend_semitones/12), cached by the setter
    float mod_wheel01_       = 0.f;
    float vibrato_phase_     = 0.f;
    float current_pitch_mult_ = 1.f; // recomputed once per sample in Process()

    daisysp::Svf filter_l_, filter_r_;
    FilterMode   filter_mode_     = FilterMode::Off;
    float        filter_cutoff01_ = 1.f;
    float        filter_res01_    = 0.f;

    float attack01_ = 0.01f, decay01_ = 0.3f, sustain01_ = 0.8f, release01_ = 0.2f;
    static constexpr float kMinAdsrSeconds = 0.005f;
    static constexpr float kMaxAdsrSeconds = 3.f;

    float output_level01_ = 0.8f;
    float output_level_   = 1.f; // powf(output_level01_,2.5f)*1.4f, cached by the setter
    float pan01_          = 0.5f;
    float pan_l_gain_     = 0.5f;
    float pan_r_gain_     = 0.5f;

    float reverb_send01_ = 0.f;
    float delay_send01_  = 0.f;

    // Same threshold/hold-seconds convention as every other clip latch
    // in this project (DexedSynth::IsLimiting(), LooperLayer::clip_hold_).
    static constexpr float kPreLimiterClipThreshold = 1.f;
    static constexpr float kClipHoldSeconds         = 0.5f;
    static constexpr float kMeterReleaseSeconds     = 0.3f;
    float pre_limiter_clip_hold_ = 0.f;
    float pre_limiter_level_     = 0.f;

    // See PlayPreview()'s own comment.
    bool  preview_active_ = false;
    float preview_pos_    = 0.f;
};
