#pragma once
#include <cstddef>
#include <cstdint>
#include "daisysp.h"
#include "fast_chorus.h"
#include "synth_lfo.h"

// A JP-8000-style Super Saw synth (see DESIGN.md, "Supersaw"). Per voice:
// OSC1 is seven saws detuned by the JP's own table and knob curve, mixed
// by its Mix law and high-passed at the note's pitch; OSC2 is a Saw,
// Square or Triangle with coarse/fine tuning; OSC Balance sets one against
// the other. Then a per-voice filter (LPF/HPF/BPF, 12 or 24 dB) with its
// own ADSR, depth and key follow, and an amp ADSR. One LFO for the engine
// (Tri/Saw/Square/S&H, free or tempo-synced, with a per-voice Fade) moves
// pitch, filter and amp. Poly, Mono or Mono Legato, with Portamento.
// After the voices: one shared chorus (the Ensemble's), Gain, the soft
// limiter, Level, Pan and the reverb/delay sends.
//
// Cheap like the Organ: integer phases (a saw is an add and a convert),
// PolyBLEP only on the sample a saw wraps, envelopes, LFO, glide and
// filter coefficients once every kEnvStep samples, and each voice
// rendered across the whole block with idle voices skipped.
class SupersawSynth
{
  public:
    void Init(float sample_rate);

    // Same 3-tier voice allocation and IRQ guard as EnsembleSynth (see
    // its NoteOn()). In Mono/Legato every note goes to voice 0.
    void NoteOn(uint8_t note, uint8_t velocity);
    void NoteOff(uint8_t note);
    void AllNotesOff();
    void SetPitchBend14bit(uint16_t raw14); // +-2 semitones, as every engine
    void SetModWheel01(float v01);

    enum class ModWheelTarget
    {
        Off,
        Detune,
        FilterCutoff,
        OutputLevel,
        kCount
    };
    void           SetModWheelTarget(ModWheelTarget t) { mod_wheel_target_ = t; }
    ModWheelTarget GetModWheelTarget() const { return mod_wheel_target_; }
    void           CycleModWheelTarget()
    {
        mod_wheel_target_
            = (ModWheelTarget)(((int)mod_wheel_target_ + 1) % (int)ModWheelTarget::kCount);
    }
    const char* GetModWheelTargetName() const
    {
        switch(mod_wheel_target_)
        {
            case ModWheelTarget::Detune: return "Detune";
            case ModWheelTarget::FilterCutoff: return "Cutoff";
            case ModWheelTarget::OutputLevel: return "Level";
            default: return "Off";
        }
    }

    // The tempo clock, once per block from the audio callback, for a synced
    // LFO: beat position (loop position while the looper runs, so the LFO
    // lines up with bar 1) and whether that position is the loop's.
    void SetClock(float beat_pos, float bpm, bool locked) { lfo_.SetClock(beat_pos, bpm, locked); }

    void Process(size_t size, float* out_l, float* out_r, float* reverb_send_l,
                 float* reverb_send_r, float* delay_send_l, float* delay_send_r);

    // --- OSC1: Super Saw ---------------------------------------------
    void  SetDetune01(float v01);
    float GetDetune01() const { return detune01_; }
    void  SetMix01(float v01);
    float GetMix01() const { return mix01_; }
    // How far the outer pair of saws sits from the centre one, in cents,
    // at the current Detune (the Super Saw page's "Spread").
    float GetSpreadCents() const;

    // --- OSC2 --------------------------------------------------------
    enum class Osc2Wave
    {
        Saw,
        Square,
        Triangle,
        kCount
    };
    void     SetOsc2Wave(Osc2Wave w) { osc2_wave_ = w; }
    Osc2Wave GetOsc2Wave() const { return osc2_wave_; }
    void     SetOsc2Coarse(int semis); // -24..+24
    int      GetOsc2Coarse() const { return osc2_coarse_; }
    void     SetOsc2Fine(float cents); // -50..+50
    float    GetOsc2Fine() const { return osc2_fine_; }
    // 0 = OSC1 only, 0.5 = both at full, 1 = OSC2 only.
    void  SetBalance01(float v01);
    float GetBalance01() const { return balance01_; }

    // --- Filter ------------------------------------------------------
    enum class FilterType
    {
        LowPass,
        HighPass,
        BandPass,
        kCount
    };
    void       SetFilterType(FilterType t) { filter_type_ = t; }
    FilterType GetFilterType() const { return filter_type_; }
    void       SetFilter24dB(bool on) { filter_24db_ = on; }
    bool       GetFilter24dB() const { return filter_24db_; }
    void       SetCutoff01(float v01);
    float      GetCutoff01() const { return cutoff01_; }
    void       SetResonance01(float v01);
    float      GetResonance01() const { return res01_; }
    // Envelope depth, 0.5 = none, 0 / 1 = -/+ kFilterEnvOctaves.
    void  SetFilterEnvDepth01(float v01);
    float GetFilterEnvDepth01() const { return fenv_depth01_; }
    // 0 = cutoff fixed, 1 = cutoff follows the keyboard one octave per octave.
    void  SetKeyFollow01(float v01);
    float GetKeyFollow01() const { return key_follow01_; }

    // --- Envelopes (0..1 knobs, 2 ms .. 4 s like the Ensemble's) -----
    enum Env
    {
        kFilterEnv,
        kAmpEnv,
        kNumEnvs
    };
    void  SetAttack01(int env, float v01);
    void  SetDecay01(int env, float v01);
    void  SetSustain01(int env, float v01);
    void  SetRelease01(int env, float v01);
    float GetAttack01(int env) const { return adsr_[env][0]; }
    float GetDecay01(int env) const { return adsr_[env][1]; }
    float GetSustain01(int env) const { return adsr_[env][2]; }
    float GetRelease01(int env) const { return adsr_[env][3]; }
    static float EnvSeconds(float v01); // attack, decay and release

    // --- LFO (see synth_lfo.h) ---------------------------------------
    SynthLfo&       Lfo() { return lfo_; }
    const SynthLfo& Lfo() const { return lfo_; }
    using LfoWave                  = SynthLfo::Wave;
    static constexpr int kNumSyncs = SynthLfo::kNumSyncs;
    enum LfoDest
    {
        kLfoPitch    = SynthLfo::kPitch,
        kLfoFilter   = SynthLfo::kFilter,
        kLfoAmp      = SynthLfo::kAmp,
        kNumLfoDests = SynthLfo::kNumDests
    };
    void        SetLfoWave(LfoWave w) { lfo_.SetWave(w); }
    LfoWave     GetLfoWave() const { return lfo_.GetWave(); }
    void        SetLfoRate01(float v01) { lfo_.SetRate01(v01); }
    float       GetLfoRate01() const { return lfo_.GetRate01(); }
    float       GetLfoRateHz() const { return lfo_.GetRateHz(); }
    void        SetLfoSync(bool on) { lfo_.SetSync(on); }
    bool        GetLfoSync() const { return lfo_.GetSync(); }
    void        SetLfoSyncIndex(int i) { lfo_.SetSyncIndex(i); }
    int         GetLfoSyncIndex() const { return lfo_.GetSyncIndex(); }
    static const char* SyncName(int i) { return SynthLfo::SyncName(i); }
    void        SetLfoFade01(float v01) { lfo_.SetFade01(v01); }
    float       GetLfoFade01() const { return lfo_.GetFade01(); }
    void        SetLfoDepth01(int dest, float v01) { lfo_.SetDepth01(dest, v01); }
    float       GetLfoDepth01(int dest) const { return lfo_.GetDepth01(dest); }

    // --- Voice -------------------------------------------------------
    enum class VoiceMode
    {
        Poly,
        Mono,
        Legato,
        kCount
    };
    void      SetVoiceMode(VoiceMode m);
    VoiceMode GetVoiceMode() const { return voice_mode_; }
    void      SetPortamento(bool on) { porta_on_ = on; }
    bool      GetPortamento() const { return porta_on_; }
    void      SetPortaTime01(float v01); // 0..2 s, constant time
    float     GetPortaTime01() const { return porta_time01_; }

    // Velocity amount, 0..1: 0 plays every note alike. At 1 the level
    // follows the usual DLS/MIDI curve (velocity squared: 40 is about 19 dB
    // under 120); in between, that much of it. A note
    // played harder than the JP's standard velocity (80) is louder,
    // brighter (the cutoff opens, and the filter envelope sweeps further)
    // and snappier (both envelopes' attacks shorten); softer is quieter,
    // darker and slower to swell. Set at each note's start.
    void  SetVelSens01(float v01);
    float GetVelSens01() const { return vel_sens01_; }

    static constexpr int kMaxVoices     = 16;
    static constexpr int kDefaultVoices = 10;
    void                 SetMaxVoices(int n);
    int                  GetMaxVoices() const { return max_voices_; }

    // --- Chorus, mix and sends (as on the Ensemble) ------------------
    void  SetChorusDepth01(float v01);
    float GetChorusDepth01() const { return chorus_depth01_; }
    void  SetChorusRate01(float v01);
    float GetChorusRate01() const { return chorus_rate01_; }

    void  SetGain01(float v01);
    float GetGain01() const { return gain01_; }
    void  SetOutputLevel01(float v01);
    float GetOutputLevel01() const { return output_level01_; }
    void  SetPan01(float v01);
    float GetPan01() const { return pan01_; }
    void  SetReverbSend01(float v01) { reverb_send01_ = v01; }
    float GetReverbSend01() const { return reverb_send01_; }
    void  SetDelaySend01(float v01) { delay_send01_ = v01; }
    float GetDelaySend01() const { return delay_send01_; }
    bool  IsLimiting() const { return pre_limiter_clip_hold_ > 0.f; }
    float GetPreLimiterLevel01() const { return pre_limiter_level_; }

    // A flat snapshot of the sound, as the other engines' preset data: a
    // file format (PerformanceStore's SUPERSAW/ presets, snapshots,
    // performances), so it only ever grows by appending fields.
    struct SupersawPresetData
    {
        float   detune01       = 0.5f;
        float   mix01          = 0.7f;
        int32_t osc2_wave      = 0; // Osc2Wave
        int32_t osc2_coarse    = 0;
        float   osc2_fine      = 0.f;
        float   balance01      = 0.f;
        int32_t filter_type    = 0; // FilterType
        int32_t filter_24db    = 1;
        float   cutoff01       = 0.85f;
        float   res01          = 0.1f;
        float   fenv_depth01   = 0.5f;
        float   key_follow01   = 0.f;
        float   adsr[kNumEnvs][4] = {{0.f, 0.3f, 1.f, 0.35f}, {0.f, 0.3f, 1.f, 0.35f}};
        int32_t lfo_wave       = 0; // LfoWave
        float   lfo_rate01     = 0.4f;
        int32_t lfo_sync       = 0;
        int32_t lfo_sync_index = 2;
        float   lfo_fade01     = 0.f;
        float   lfo_depth01[kNumLfoDests] = {0.5f, 0.5f, 0.5f};
        int32_t voice_mode     = 0; // VoiceMode
        int32_t porta_on       = 0;
        float   porta_time01   = 0.3f;
        float   chorus_depth01 = 0.35f;
        float   chorus_rate01  = 0.3f;
        float   gain01         = 0.f; // New: 0 dB, as every other engine starts
        float   output_level01 = 0.7f;
        float   pan01          = 0.5f;
        float   reverb_send01  = 0.3f; // a little space from the start
        float   delay_send01   = 0.f;
        // -1 = not stored (factory sounds): loading leaves it alone.
        int32_t mod_wheel_target = -1;
        // The Ui's MIDI channel (-1 = Omni), filled in on a user save;
        // -2 = not stored.
        int32_t midi_channel = -2;
        int32_t max_voices   = -1; // -1 = not stored
        float   vel_sens01   = 0.5f;
    };
    void               ApplyPreset(const SupersawPresetData& p);
    SupersawPresetData CapturePreset() const;
    SupersawPresetData GetNewPreset() const { return SupersawPresetData{}; }

    // Factory sounds: parameter numbers only (no audio), so ours to give
    // away. See supersaw_synth.cpp.
    static int                GetNumFactoryPresets();
    static const char*        GetFactoryPresetName(int i);
    static SupersawPresetData GetFactoryPreset(int i);

  private:
    struct Voice
    {
        // OSC1's seven saws then OSC2, each a phase that wraps by itself.
        uint32_t phase[8] = {};
        uint32_t inc[8]   = {};
        float    inv_inc[8] = {}; // 1 / inc, for the PolyBLEP
        float    eff_pitch  = -1000.f; // the pitch inc[] was set for
        // OSC1's high-pass at the note's pitch: one pole.
        float hp_coef = 0.f, hp_x1 = 0.f, hp_y1 = 0.f;
        // Filter: two trapezoidal SVF stages (the second only at 24 dB).
        float f_ic1[2] = {}, f_ic2[2] = {};
        daisysp::Adsr fenv, aenv;
        float         amp_last = 0.f; // for the ramp between envelope steps
        float         fade     = 0.f; // LFO fade-in, 0..1
        // Pitch in semitones (MIDI note numbers), gliding to target.
        float pitch      = 60.f;
        float pitch_to   = 60.f;
        float glide_step = 0.f;
        float vel_scale  = 1.f; // filter-env depth, from velocity (see SetVelSens01())
        float vel_amp    = 1.f; // level, from velocity
        float vel_oct    = 0.f; // cutoff shift in octaves, from velocity
        int   held_note  = -1; // -1 = free
        int   pitch_note = 60;
        uint32_t triggered_at = 0;
    };

    int  FindVoiceForNote(uint8_t note);
    void StartVoice(Voice& v, int note, uint8_t velocity, bool retrigger, bool glide);
    void SetVoiceIncrements(Voice& v, float pitch_semis);
    void UpdateDetuneRatios();

    Voice    voices_[kMaxVoices];
    int      max_voices_   = kDefaultVoices;
    uint32_t trigger_seq_  = 0;
    float    last_pitch_   = -1.f; // where a glide starts from; -1 = none yet
    // Mono/Legato: the keys held, oldest first (last-note priority).
    static constexpr int kStack = 16;
    uint8_t stack_[kStack] = {};
    int     stack_n_       = 0;
    uint8_t mono_vel_      = 100; // the last note's velocity, for going back to a held key

    float sample_rate_ = 48000.f;

    float detune01_ = 0.5f, mix01_ = 0.7f;
    float detune_eff_ = -1.f;  // the Detune these ratios were built for
    float ratio_[8]     = {};  // each saw's pitch, x the centre; [7] is OSC2
    float inv_ratio_[8] = {};
    float osc2_eff_     = -1000.f; // the OSC2 tuning ratio_[7] was set for
    float side_gain_  = 0.f;   // each side saw, from Mix
    Osc2Wave osc2_wave_   = Osc2Wave::Saw;
    int      osc2_coarse_ = 0;
    float    osc2_fine_   = 0.f;
    float    balance01_   = 0.f;

    FilterType filter_type_  = FilterType::LowPass;
    bool       filter_24db_  = true;
    float      cutoff01_     = 0.85f;
    float      res01_        = 0.1f;
    float      fenv_depth01_ = 0.5f;
    float      key_follow01_ = 0.f;

    float adsr_[kNumEnvs][4] = {{0.f, 0.3f, 1.f, 0.35f}, {0.f, 0.3f, 1.f, 0.35f}};

    SynthLfo lfo_;

    float     vel_sens01_   = 0.5f;
    VoiceMode voice_mode_   = VoiceMode::Poly;
    bool      porta_on_     = false;
    float     porta_time01_ = 0.3f;

    float          pitch_bend_ = 0.f; // semitones
    ModWheelTarget mod_wheel_target_ = ModWheelTarget::Off;
    float          mod_wheel01_      = 0.f;

    FastChorus      chorus_;
    float           chorus_depth01_        = 0.35f;
    float           chorus_depth_smoothed_ = 0.35f;
    float           chorus_rate01_         = 0.3f;

    float gain01_ = 0.f, gain_ = 1.f;
    float output_level01_ = 0.7f, output_level_ = 1.f;
    float pan01_ = 0.5f, pan_l_gain_ = 0.7071f, pan_r_gain_ = 0.7071f;
    float reverb_send01_ = 0.3f, delay_send01_ = 0.f;

    float pre_limiter_clip_hold_ = 0.f;
    float pre_limiter_level_     = 0.f;
    float last_block_peak_       = 0.f;
};
