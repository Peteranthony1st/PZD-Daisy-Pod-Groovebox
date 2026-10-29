#pragma once
#include <cstdint>
#include "svf_setup.h"
#include <cstddef>
#include "daisysp.h"
#include "looper_layer.h" // FilterMode, kFilterMinHz/kFilterMaxHz
#include "ensemble_factory_data.h"

// A divide-down string-machine/ensemble-organ instrument (daisysp::
// OscillatorBank -- one master oscillator's phase divided into 7
// octave-related saw/square components, "8'/4'/2'/1'" organ-stop
// terminology, the same lineage as the Solina String Ensemble; ported
// from Plaits' own string_synth_oscillator). Voice allocation is a
// direct port of DexedSynth::FindVoiceForNote()'s own 3-tier shape
// (retrigger / prefer-idle-free / oldest-steal), substituting
// OscillatorBank's own lack of an envelope for an explicitly-wrapped
// daisysp::Adsr per voice (DexedSynth gets "is this voice idle" for
// free from msfa's own envelope; OscillatorBank is a continuously-
// running oscillator with no note lifecycle of its own, so that has to
// come from somewhere -- SampleSynth::FindVoiceForNote() already uses
// exactly this `!adsr.IsRunning()` substitution for the same reason,
// so this mirrors that, not a new idea).
//
// Headroom: a fixed scale constant plus a tanhf() soft limiter, same
// shape as DexedSynth::Process() actually ships today (its own dynamic
// per-held-note 1/sqrt(N) compensation exists in that code but is
// switched off -- kEnableVoiceHeadroomCompensation = false -- so this
// matches what Dexed really does, not the disabled alternate path).
//
// Real, isolated hardware measurement (see git history: a throwaway
// osc_bank_test.cpp on the physical-modelling-synth branch) found 8
// voices + one shared Chorus costs ~19-23% peak CPU -- the voice count
// and the single-shared-Chorus-not-per-voice design below both come
// directly from that measurement, not an estimate.
class EnsembleSynth
{
  public:
    void Init(float sample_rate);

    // Same 3-tier allocation as DexedSynth::NoteOn() -- see its own
    // comment for the full race-condition reasoning (this runs from
    // ControlTimerCallback, a lower NVIC priority than the audio ISR,
    // which can genuinely preempt a multi-field voice write mid-
    // mutation under fast playing).
    void NoteOn(uint8_t note, uint8_t velocity);
    void NoteOff(uint8_t note);
    // MIDI CC 123 (All Notes Off)/120 (All Sound Off) -- same real-world
    // need as DexedSynth::AllNotesOff() (see its own comment, including
    // the confirmed-on-hardware Akai MPK Mini Plus arp repro): releases
    // every currently-held voice regardless of note number.
    void AllNotesOff();

    // Raw 14-bit MIDI pitch bend (0..16383, 8192 = centered) -- same
    // convention and range (+-2 semitones) as DexedSynth::
    // SetPitchBend14bit(), for consistency across engines. Bends every
    // currently-held voice's real pitch immediately; NoteOn() also
    // applies whatever bend is already in effect to a freshly-struck
    // voice, same as a real synth's pitch wheel affecting new notes
    // struck while it's off-center.
    void SetPitchBend14bit(uint16_t raw14);

    // 0..1, same convention as every other engine's own mod-wheel-style
    // input. Off by default (ModWheelTarget::Off) -- explicitly picked,
    // not assumed -- and even once a target is picked, wheel=0 changes
    // nothing (see ModWheelTarget's own comment for the exact shape).
    void SetModWheel01(float v01);
    // Which parameter the wheel modulates -- deliberately NOT
    // overriding independent settings (e.g. picking FilterCutoff does
    // NOT force Filter Mode on if it's currently Off): the wheel always
    // modulates whatever's already dialed in on the matching knob/page,
    // pushing it up from its own current value toward 1.0 as the wheel
    // rises (`effective = base + wheel01*(1-base)`), same "layered on
    // top of the base setting" relationship DexedSynth's own Vibrato
    // Depth + wheel Pitch target already has -- not a separate,
    // independently-ranged sweep the way an earlier design draft of
    // this considered and explicitly rejected.
    enum class ModWheelTarget
    {
        Off,
        ChorusDepth,
        OutputLevel,
        FilterCutoff
    };
    void CycleModWheelTarget()
    {
        mod_wheel_target_ = (ModWheelTarget)(((int)mod_wheel_target_ + 1) % 4);
    }
    void SetModWheelTarget(ModWheelTarget t) { mod_wheel_target_ = (ModWheelTarget)((int)t % 4); }
    ModWheelTarget GetModWheelTarget() const { return mod_wheel_target_; }
    const char*    GetModWheelTargetName() const
    {
        switch(mod_wheel_target_)
        {
            case ModWheelTarget::Off: return "Off";
            case ModWheelTarget::ChorusDepth: return "Chorus";
            case ModWheelTarget::OutputLevel: return "Level";
            default: return "Filter";
        }
    }

    // Same WRITES-not-adds / reverb-send-ADDS convention as every other
    // engine's own Process() in this project.
    void Process(size_t size, float* out_l, float* out_r, float* reverb_send_l,
                 float* reverb_send_r, float* delay_send_l, float* delay_send_r);

    void  SetReverbSend01(float v01) { reverb_send01_ = v01; }
    float GetReverbSend01() const { return reverb_send01_; }
    void  SetDelaySend01(float v01) { delay_send01_ = v01; }
    float GetDelaySend01() const { return delay_send01_; }
    void  SetOutputLevel01(float v01);
    float GetOutputLevel01() const { return output_level01_; }
    // Gain into the soft limiter, 0 to +12 dB (1x to 4x). Ensemble's
    // loudness follows how many drawbars are up -- a thin registration
    // is genuinely quiet, and Output Level (applied AFTER the limiter)
    // can't make that up without clipping further down the chain. This
    // brings it up front instead; pushed hard, the limiter starts to
    // saturate gently, which suits an organ/string machine.
    void  SetGain01(float v01)
    {
        gain01_ = v01 < 0.f ? 0.f : (v01 > 1.f ? 1.f : v01);
        gain_   = powf(4.f, gain01_);
    }
    float GetGain01() const { return gain01_; }
    void  SetPan01(float v01);
    float GetPan01() const { return pan01_; }

    // Same PRE-limiter tap point/reasoning as DexedSynth::IsLimiting()/
    // GetPreLimiterLevel01() -- Screen::Mixer's own clip indicator/Peak
    // bar need this, not a post-tanhf()/post-output_level_/post-pan
    // check (see Process(), where this is tracked against max(|l|,|r|)
    // right before the limiter -- genuinely stereo here, unlike Dexed's
    // own mono-then-panned signal, so the louder of the two channels is
    // what actually risks clipping).
    bool  IsLimiting() const { return pre_limiter_clip_hold_ > 0.f; }
    float GetPreLimiterLevel01() const { return pre_limiter_level_; }

    // Post-mix bus filter -- same Cutoff/Resonance/button-cycled-mode
    // convention every other engine's own bus filter uses (e.g.
    // DexedSynth::SetFilterMode() and friends).
    void       SetFilterMode(FilterMode m) { filter_mode_ = m; }
    FilterMode GetFilterMode() const { return filter_mode_; }
    void       SetFilterCutoff01(float v01) { filter_cutoff01_ = v01; }
    float      GetFilterCutoff01() const { return filter_cutoff01_; }
    void       SetFilterResonance01(float v01) { filter_res01_ = v01; }
    float      GetFilterResonance01() const { return filter_res01_; }

    // The signature control -- 7 real drawbars (organ-stop terminology:
    // 0=Saw8', 1=Sq8', 2=Saw4', 3=Sq4', 4=Saw2', 5=Sq2', 6=Saw1'),
    // directly manually editable on-device (UI: EnsembleParamPage::
    // Registration, K1 selects which drawbar 0-6, K2 sets its level) --
    // a real per-user-request change from an earlier single macro-sweep
    // design, which turned out both too coarse and (combined with a
    // real daisysp::OscillatorBank bug, since fixed) too subtle to
    // confirm as working on real hardware. Each level is independent
    // 0..1, same as a real Hammond-style drawbar slider -- not
    // normalized/forced to sum to 1 the way the two-registration-preset
    // macro design required, so setting these freely (including all 7
    // near 1) can genuinely drive the mix louder; the per-voice
    // kVoiceGain margin plus the final tanhf() limiter (see Process())
    // are the safety net for that, same as every other engine's own
    // fixed-headroom-plus-limiter approach.
    void  SetDrawbarLevel(int idx, float level01);
    float GetDrawbarLevel(int idx) const;
    static constexpr int kNumDrawbars = 7;

    // Chorus/ensemble depth and rate -- the one shared Chorus instance
    // applied to the summed mix (not per voice, see the class's own
    // doc comment above for why).
    void  SetChorusDepth01(float v01);
    float GetChorusDepth01() const { return chorus_depth01_; }
    void  SetChorusRate01(float v01);
    float GetChorusRate01() const { return chorus_rate01_; }

    // Same explicit-Button-target-toggle convention
    // SampleSynthParamPage::Env/DexedParamPage::FX already use: one
    // pair of knobs, Button1/Button2 pick which pair of fields they
    // currently move.
    void  SetAttack01(float v01);
    float GetAttack01() const { return attack01_; }
    void  SetDecay01(float v01);
    float GetDecay01() const { return decay01_; }
    void  SetSustain01(float v01);
    float GetSustain01() const { return sustain01_; }
    void  SetRelease01(float v01);
    float GetRelease01() const { return release01_; }
    // Real seconds, same 2ms-4s exponential curve Set*01() above
    // already uses -- for DrawAdsrShape()'s own envelope graphic
    // (Ui::DrawEnsembleScreen(), same shape GranularParamPage::ADSR
    // already shows), which needs actual seconds, not the raw 0..1
    // knob value. Declared here, defined in the .cpp (needs powf()),
    // same split GranularEngine::GetAttackSeconds() and friends use.
    float GetAttackSeconds() const;
    float GetDecaySeconds() const;
    float GetReleaseSeconds() const;

    // A flat POD snapshot of the currently-loaded sound -- same
    // "preset != session mix" split every other engine's own
    // XxxPresetData already uses. No name field -- names live at the
    // file/slot level (PerformanceStore::SaveEnsemblePreset()'s own
    // `name` parameter), matching SaveSampleSynthKit()'s own split.
    struct EnsemblePresetData
    {
        // Same default registration Init() itself starts with (a warm,
        // fundamental-heavy tone) -- see kDefaultDrawbars.
        float   drawbar_levels[kNumDrawbars]
            = {0.60f, 0.00f, 0.25f, 0.00f, 0.10f, 0.00f, 0.05f};
        float   chorus_depth01       = 0.5f;
        float   chorus_rate01        = 0.3f;
        float   attack01             = 0.1f;
        float   decay01              = 0.3f;
        float   sustain01            = 0.8f;
        float   release01            = 0.3f;
        int32_t filter_mode          = (int32_t)FilterMode::Off;
        float   filter_cutoff01      = 1.f;
        float   filter_res01         = 0.f;
        float   output_level01       = 0.70f;
        float   pan01                = 0.5f;
        float   reverb_send01        = 0.f;
        float   delay_send01         = 0.f;
        // Added in preset file v2 -- 0 = unity (how every v1 preset
        // sounded), see SetGain01().
        float   gain01               = 0.f;
        // Added in preset file v3. -1 = not stored (factory presets and
        // older files): loading leaves the current target alone.
        int32_t mod_wheel_target     = -1;
        // The Ui's own Ensemble MIDI channel (-1 = Omni), filled in by
        // the Ui on a user save -- CapturePreset() never sets it. -2 =
        // not stored: loading leaves the current channel alone.
        int32_t midi_channel         = -2;
    };
    void ApplyPreset(const EnsemblePresetData& p)
    {
        for(int i = 0; i < kNumDrawbars; i++)
            SetDrawbarLevel(i, p.drawbar_levels[i]);
        SetChorusDepth01(p.chorus_depth01);
        SetChorusRate01(p.chorus_rate01);
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
        SetGain01(p.gain01);
        if(p.mod_wheel_target >= 0)
            SetModWheelTarget((ModWheelTarget)p.mod_wheel_target);
    }
    EnsemblePresetData CapturePreset() const;

    // Factory presets -- pure parameter numbers (no audio at all), so
    // genuinely zero licensing concern, same as Dexed's own factory
    // bank. See ensemble_factory_data.h/.cpp.
    static int                 GetNumFactoryPresets();
    static const char*         GetFactoryPresetName(int flat_index);
    static EnsemblePresetData  GetFactoryPreset(int flat_index);

    // Chosen from real, isolated hardware measurement (see the class's
    // own doc comment above) -- not a guess or a value carried over
    // from another engine.
    static constexpr int kMaxVoices = 8;

  private:
    int  FindVoiceForNote(uint8_t note);

    struct Voice
    {
        daisysp::OscillatorBank osc;
        daisysp::Adsr           adsr;
        int      held_note   = -1; // -1 = free
        uint8_t  velocity     = 100;
        uint32_t triggered_at = 0;
    };
    Voice    voices_[kMaxVoices];
    uint32_t trigger_seq_ = 0;

    // 1.0 = centered/no bend -- see SetPitchBend14bit()'s own comment.
    float pitch_bend_ratio_ = 1.f;

    ModWheelTarget mod_wheel_target_ = ModWheelTarget::Off;
    float          mod_wheel01_      = 0.f;

    // The 7 real drawbar levels -- see SetDrawbarLevel()'s own comment.
    // Shared across all 8 voices (every voice's own osc.SetAmplitudes()
    // gets the same registration), cached here so Process() doesn't
    // need to rebuild the array per voice.
    float drawbar_levels_[kNumDrawbars] = {0.60f, 0.60f, 0.60f, 0.60f, 0.60f, 0.60f, 0.60f};

    // ONE shared instance, not one per voice -- see the class's own
    // doc comment for the real measured reason. Genuinely stereo
    // (GetLeft()/GetRight()), not collapsed back to mono -- the actual
    // point of including it at all was real ensemble width, not just a
    // mono color filter.
    daisysp::Chorus chorus_;
    float            chorus_depth01_ = 0.5f;
    float            chorus_rate01_  = 0.3f;

    float attack01_  = 0.1f;
    float decay01_   = 0.3f;
    float sustain01_ = 0.8f;
    float release01_ = 0.3f;

    float sample_rate_   = 48000.f;
    float reverb_send01_ = 0.f;
    float delay_send01_  = 0.f;

    // Same curve every other engine's own Output Level uses
    // (GranularEngine::SetOutputLevel01() etc.).
    float gain01_ = 0.f;
    float gain_   = 1.f; // powf(4, gain01_), cached by SetGain01()
    float output_level01_ = 0.70f;
    float output_level_   = 1.f; // powf(output_level01_, 2.5f)*1.4f, cached by the setter

    // See IsLimiting()'s own comment. Checked in Process() against
    // max(|l|,|r|) right before the limiter, same threshold/hold-
    // seconds convention as DexedSynth's own pre_limiter_clip_hold_.
    float pre_limiter_clip_hold_ = 0.f;
    // Loudest output sample of the last block -- Process() only skips while
    // this is silent, so a filter/chorus/fold tail is never cut off.
    float last_block_peak_ = 0.f;
    float pre_limiter_level_     = 0.f; // see GetPreLimiterLevel01()

    float pan01_      = 0.5f;
    float pan_l_gain_ = 0.7071f; // EqualPowerPan(pan01_), control-rate cache
    float pan_r_gain_ = 0.7071f;

    // Stereo, unlike Dexed's own mono-then-panned filter pair -- each
    // channel processes its own real signal (post-Chorus, genuinely
    // different L/R content), not the same mono input twice.
    TptSvf filter_l_, filter_r_;
    FilterMode   filter_mode_     = FilterMode::Off;
    float        filter_cutoff01_ = 1.f;
    float        filter_res01_    = 0.f;

    // Fixed per-voice headroom margin, applied once via each voice's
    // own OscillatorBank::SetGain() -- same spirit as Dexed's own fixed
    // kHeadroomScale (a measured-conservative margin plus the tanhf()
    // limiter as the safety net, no dynamic per-held-note awareness),
    // just applied per-voice here rather than as a post-sum multiplier,
    // since OscillatorBank's own Process() already returns a plain
    // float rather than a raw fixed-point value needing a scale
    // conversion the way msfa's own Q24 output does.
    //
    // Untuned starting point (today's own isolated test needed 0.08/
    // voice for 8 voices with no envelope/filter/chorus on top yet --
    // this is a deliberately conservative guess pending the real
    // combined-chain measurement the plan's own Verification step
    // calls for, not a final value).
    static constexpr float kVoiceGain = 0.08f;
};
