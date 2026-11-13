#pragma once
#include <cstdint>
#include "svf_setup.h"
#include <cstddef>
#include "daisysp.h"
#include "looper_layer.h" // FilterMode, kFilterMinHz/kFilterMaxHz
#include "msfa/dx7note.h"
#include "msfa/controllers.h"
#include "msfa/EngineMsfa.h"
#include "msfa/fm_core.h"
#include "msfa/lfo.h"
#include "dexed_factory_data.h"

// The shared Dexed output bus. DXD-A and DXD-B (two DexedSynth
// engines) each add their own voices, already at their own Level and
// Pan, into one stereo pair; this then runs the rest of what used to be
// DexedSynth's single output stage over the sum, once: bus filter,
// wavefolder, Gain into the soft limiter, Level, Pan and the reverb/
// delay sends. One filter and fold for both engines, not one each, is
// the CPU reason for a shared bus.
//
// With both engines' own Pan centred the two channels are identical,
// and Process() takes the mono path: exactly the old single-engine
// signal chain, one filter, nothing extra. Only an engine panned off
// centre costs the second filter.
class DexedBus
{
  public:
    void Init(float sample_rate);

    // In place: l/r hold the summed engine output on entry and the
    // finished bus output on return. `active` is false when no engine
    // added anything this block, so a silent bus with no tail left can
    // skip the per-sample work. Sends ADD, same as every engine.
    void Process(size_t size, float* l, float* r, bool mono, bool active, float* reverb_send_l,
                 float* reverb_send_r, float* delay_send_l, float* delay_send_r);

    void  SetReverbSend01(float v01) { reverb_send01_ = v01; }
    float GetReverbSend01() const { return reverb_send01_; }
    void  SetDelaySend01(float v01) { delay_send01_ = v01; }
    float GetDelaySend01() const { return delay_send01_; }
    void  SetOutputLevel01(float v01);
    float GetOutputLevel01() const { return output_level01_; }
    void  SetGain01(float v01)
    {
        gain01_ = v01 < 0.f ? 0.f : (v01 > 1.f ? 1.f : v01);
        gain_   = powf(4.f, gain01_);
    }
    float GetGain01() const { return gain01_; }
    void  SetPan01(float v01);
    float GetPan01() const { return pan01_; }

    void       SetFilterMode(FilterMode m) { filter_mode_ = m; }
    FilterMode GetFilterMode() const { return filter_mode_; }
    void       SetFilterCutoff01(float v01) { filter_cutoff01_ = v01; }
    float      GetFilterCutoff01() const { return filter_cutoff01_; }
    void       SetFilterResonance01(float v01) { filter_res01_ = v01; }
    float      GetFilterResonance01() const { return filter_res01_; }

    void  SetFoldAmount01(float v01);
    float GetFoldAmount01() const { return fold_amount01_; }
    void  SetFoldTrim01(float v01);
    float GetFoldTrim01() const { return fold_trim01_; }

    // See DexedSynth::IsLimiting()/GetPreLimiterLevel01() -- read here,
    // before the limiter, now that the limiter is on the bus.
    bool  IsLimiting() const { return pre_limiter_clip_hold_ > 0.f; }
    float GetPreLimiterLevel01() const { return pre_limiter_level_; }

  private:
    float sample_rate_ = 48000.f;

    float reverb_send01_  = 0.f;
    float delay_send01_   = 0.f;
    float output_level01_ = 0.70f;
    float output_level_   = 1.f; // powf(output_level01_, 2.5f)*1.4f, cached by the setter

    float pre_limiter_clip_hold_ = 0.f;
    float last_block_peak_       = 0.f;
    float pre_limiter_level_     = 0.f;

    float pan01_      = 0.5f;
    float pan_l_gain_ = 0.7071f; // EqualPowerPan(pan01_), control-rate cache
    float pan_r_gain_ = 0.7071f;

    // filter_l_ alone on the mono path; filter_r_ only runs when an
    // engine is panned off centre (see the class comment).
    TptSvf     filter_l_;
    TptSvf     filter_r_;
    // Last block's path. filter_r_ sits idle on the mono path, so on the
    // switch to stereo it takes filter_l_'s state rather than restarting
    // from wherever it stopped (a click with a resonant filter).
    bool       last_mono_ = true;
    FilterMode filter_mode_     = FilterMode::Off;
    float      filter_cutoff01_ = 1.f;
    float      filter_res01_    = 0.f;

    daisysp::Wavefolder fold_; // stateless, so one serves both channels
    float fold_amount01_ = 0.f;
    float fold_trim01_   = 1.f;
    float gain01_        = 0.f;
    float gain_          = 1.f; // powf(4, gain01_), cached by SetGain01()
    float fold_trim_     = 1.f; // cached scalar, see SetFoldTrim01()
};

// DX7/msfa port (see the approved plan). Phase 2 measured real per-voice
// CPU cost on hardware (1/2/8/10 simultaneous voices, ~7% fixed + ~2.3%/
// voice, no cache-thrashing surprise even at 10) before committing to a
// voice count -- 10 was chosen from that real measurement (~76%
// estimated combined worst case alongside the looper's own real worst
// case), matching this project's own established "measure before
// committing" convention. Phase 4 wires real MIDI dispatch/engine-select
// gating; Phase 5 adds UI. Still exactly one hardcoded test patch
// (Phase 6 adds real preset save/load, Phase 7 real SysEx import).
//
// Owns Dx7Note instances directly (msfa's real Apache-2.0 note engine,
// see msfa/dx7note.h) -- everything else in this class is original code
// written against that engine's public interface, not ported from the
// original Dexed plugin's own (GPLv3, excluded) voice-pool/SysEx layer.
// The oldest-note-steal allocation below mirrors the SHAPE of the
// removed FmSynth::FindVoiceForNote()'s own original logic (same
// project, no license concern -- not from any GPL'd source), adapted to
// Dx7Note's own envelope-position query (VoiceStatus::ampStep) in place
// of DaisySP's Adsr::GetCurrentSegment().
class DexedSynth
{
  public:
    void Init(float sample_rate);

    // Polyphonic voice allocation: retriggers the same voice if `note`
    // is already held there; otherwise claims a free voice (preferring
    // one that's fully released/silent over one still ringing out from
    // a recent NoteOff -- see VoiceIsIdle()); otherwise steals the
    // oldest-triggered voice outright. NoteOff() marks a voice free
    // immediately (so it's eligible for reuse right away) but leaves it
    // rendering its own real release tail -- see RenderQuantum(), which
    // always computes every voice unconditionally: msfa's own internal
    // per-operator gain threshold (see EngineMsfa::render()) already
    // collapses a fully decayed voice's real per-sample cost to nearly
    // nothing, so no extra bookkeeping is needed here to get that for
    // free.
    void NoteOn(uint8_t note, uint8_t velocity);
    void NoteOff(uint8_t note);
    // MIDI CC 123 (All Notes Off)/120 (All Sound Off) -- releases every
    // currently-held voice regardless of note number, same real-world
    // need every polyphonic engine here has: some controllers/
    // arpeggiators (confirmed: an Akai MPK Mini Plus's own arp) don't
    // reliably send a matching NoteOff for every NoteOn they sent (most
    // commonly the very last note when the arp stops/the key releases
    // mid-cycle), which otherwise leaves that one voice stuck holding
    // its note forever -- this is the standard MIDI-spec panic path for
    // exactly that case. See main.cpp's MIDI dispatch.
    void AllNotesOff();

    // Raw 14-bit MIDI pitch bend (0..16383, 8192 = centered) -- this is
    // exactly the CC value Dx7Note::compute() itself expects (see
    // Controllers::values_[kControllerPitch] there), unlike a simplified
    // "bend amount in semitones" input (what the removed FastFmVoice
    // used): msfa's own pitch-bend math already combines this raw value
    // with a separate bend-range-in-semitones setting (hardcoded to +-2
    // in Init() for now -- a real Global/Dexed page control is future
    // incremental-editing scope, not this phase's), so re-deriving a
    // semitone offset here would just be undone downstream.
    void SetPitchBend14bit(uint16_t raw14) { ctrls_.values_[kControllerPitch] = raw14; }
    // 0..1, same convention as every other engine's own mod-wheel-style
    // input in this project.
    void SetModWheel01(float v01)
    {
        v01 = v01 < 0.f ? 0.f : (v01 > 1.f ? 1.f : v01);
        ctrls_.modwheel_cc = (uint8_t)(v01 * 127.f + 0.5f);
    }

    // Real DX7 hardware routing (msfa's own Controllers::wheel/FmMod, see
    // controllers.h) -- the mod wheel can target Pitch (vibrato, the
    // default), Amp (tremolo-style amplitude modulation), or EG Bias (the
    // mod source biases the envelope's own level dynamically). Real DX7
    // hardware lets these combine as a bitmask (setTarget() takes
    // pitch|amp<<1|eg<<2); this exposes them as a single Button-cycled
    // choice instead, matching the simpler "pick one" idiom every other
    // cycled control in this project already uses -- not a hardware
    // limitation, just a UI scope choice.
    enum class ModWheelTarget
    {
        Pitch,
        Amp,
        EgBias
    };
    void CycleModWheelTarget()
    {
        mod_wheel_target_ = (ModWheelTarget)(((int)mod_wheel_target_ + 1) % 3);
        ApplyModWheelTarget();
    }
    void SetModWheelTarget(ModWheelTarget t)
    {
        mod_wheel_target_ = (ModWheelTarget)((int)t % 3);
        ApplyModWheelTarget();
    }
    ModWheelTarget GetModWheelTarget() const { return mod_wheel_target_; }
    const char*    GetModWheelTargetName() const
    {
        switch(mod_wheel_target_)
        {
            case ModWheelTarget::Pitch: return "Pitch";
            case ModWheelTarget::Amp: return "Amp";
            default: return "EG";
        }
    }

    // The bus every engine renders into (see DexedBus). Must be set
    // before Init(), which loads the boot patch through it.
    void      SetBus(DexedBus* bus) { bus_ = bus; }
    DexedBus* GetBus() const { return bus_; }

    // ADDS this engine's voices into out_l/out_r, at its own Level and
    // Pan, ready for DexedBus::Process(). Returns false (and adds
    // nothing) while every voice is idle. Still goes through the small
    // staging buffer (stage_f_/stage_pos_, see dexed_synth.cpp's own
    // comment), written against whatever `size` is.
    bool Render(size_t size, float* out_l, float* out_r);

    // This engine's own Level and Pan going into the bus (the A and B
    // bars on the Mix page). Level 1.0 is unity; Pan is a balance (both
    // sides at full level in the centre), so one engine at its defaults
    // sounds exactly as Dexed did before the bus was split out.
    void  SetEngineLevel01(float v01);
    float GetEngineLevel01() const { return engine_level01_; }
    void  SetEnginePan01(float v01);
    float GetEnginePan01() const { return engine_pan01_; }
    bool  IsCentred() const { return engine_pan01_ == 0.5f; }

    // Mix/FX/Filter/Fold below are the shared bus's (see DexedBus): the
    // same calls from either engine reach the same settings.
    void  SetReverbSend01(float v01) { bus_->SetReverbSend01(v01); }
    float GetReverbSend01() const { return bus_->GetReverbSend01(); }
    void  SetDelaySend01(float v01) { bus_->SetDelaySend01(v01); }
    float GetDelaySend01() const { return bus_->GetDelaySend01(); }
    void  SetOutputLevel01(float v01) { bus_->SetOutputLevel01(v01); }
    float GetOutputLevel01() const { return bus_->GetOutputLevel01(); }
    // Gain into the soft limiter, 0 to +12 dB (1x to 4x) -- same control
    // as EnsembleSynth::SetGain01(), for patches that come out thin and
    // quiet. Applied before the pre-limiter tap, so the Mix page's meter
    // shows it.
    void  SetGain01(float v01) { bus_->SetGain01(v01); }
    float GetGain01() const { return bus_->GetGain01(); }

    // True for a hold period after the PRE-limiter signal (before
    // tanhf(), see Process()) last crossed unity -- i.e. "the limiter
    // is actually compressing hard right now," not just "the output is
    // loud." A plain post-tanhf()/post-output_level_ clip check can
    // never usefully fire here: tanhf() mathematically bounds its own
    // output to (-1,1) no matter how hard the pre-limiter signal is
    // driving it (tanhf(10.0) ~= 0.99999), and output_level_ scales
    // that down further still -- so Screen::Mixer's own Dexed clip
    // indicator needs this, not a generic level check on dexed_l/r.
    bool IsLimiting() const { return bus_->IsLimiting(); }

    // Live level for Screen::Mixer's own Peak bar -- same PRE-limiter
    // tap point as IsLimiting() above, not dexed_l/r. Reading dexed_l/r
    // directly looked permanently quiet even when genuinely driving the
    // limiter hard: it's post-tanhf() (bounded to (-1,1) regardless of
    // how hard the input is), post-output_level_ (often well under
    // unity, see its own comment), AND post-pan (halved again at dead
    // center) -- three separate attenuation stages stacked on top of
    // each other between "the engine is working hard" and what that bar
    // would have shown. This reads the signal before any of them, so a
    // genuinely hot signal now visibly reaches the top of the bar
    // instead of topping out around 20-40% of it.
    float GetPreLimiterLevel01() const { return bus_->GetPreLimiterLevel01(); }

    // Same linear pan law every other engine here uses (panL = 1-v,
    // panR = v). Added after a real user report that live Dexed played
    // disproportionately loud next to a recorded-and-played-back loop
    // layer: every other source in the mix (loop layers, Grains) goes
    // through this same center-pan attenuation before reaching the
    // master mix, but Dexed's own live output previously had no pan
    // stage at all, so it reached out[] fully unscaled while everything
    // else took the usual -6dB-at-center cut.
    void  SetPan01(float v01) { bus_->SetPan01(v01); }
    float GetPan01() const { return bus_->GetPan01(); }

    // Post-mix bus filter, same shape as GranularEngine's own (a plain
    // Svf pair, not part of the real DX7's own signal path -- this
    // project's own addition, consistent with every other engine here
    // having one).
    void       SetFilterMode(FilterMode m) { bus_->SetFilterMode(m); }
    FilterMode GetFilterMode() const { return bus_->GetFilterMode(); }
    void       SetFilterCutoff01(float v01) { bus_->SetFilterCutoff01(v01); }
    float      GetFilterCutoff01() const { return bus_->GetFilterCutoff01(); }
    void       SetFilterResonance01(float v01) { bus_->SetFilterResonance01(v01); }
    float      GetFilterResonance01() const { return bus_->GetFilterResonance01(); }

    // Wavefolder -- a true West-Coast-style fold (reflects overshoot
    // back into the waveform instead of clamping it), Dexed-only in
    // this project since FM content pairs well with it but recorded/
    // granular audio tends to just sound broken. amount01=0 maps to
    // gain=1.0 inside daisysp::Wavefolder, which is an exact unity
    // passthrough by construction (see Process()'s own comment) -- no
    // bypass-threshold guard needed, unlike the (removed) clip-
    // distortion feature this replaced.
    void  SetFoldAmount01(float v01) { bus_->SetFoldAmount01(v01); }
    float GetFoldAmount01() const { return bus_->GetFoldAmount01(); }
    // Post-fold output trim, 0..1, default 1.0 = unity (no change) --
    // tames the perceived loudness folding can add at high Amount.
    void  SetFoldTrim01(float v01) { bus_->SetFoldTrim01(v01); }
    float GetFoldTrim01() const { return bus_->GetFoldTrim01(); }

    // Bipolar macro knobs, centered at "as stored in the preset" (0.5),
    // matching the soft-pickup convention every other knob in this
    // project uses -- both scale linearly from 0x at v01=0 to 1x
    // (unchanged) at v01=0.5 to 2x at v01=1. Directly mutate patch_
    // (relative to patch_baseline_, the pristine values captured at the
    // last SetPatch() call) and push the result to any currently-held
    // voice via Dx7Note::update() (not init()), so turning either while
    // holding a note doesn't click or retrigger it. Saving a preset
    // captures patch_ as-is -- these are real, persistent edits, not a
    // transient overlay that resets on save (matches how every other
    // knob/preset in this project already works).
    //
    // Brightness scales the output level of every operator NOT in
    // FmCore::get_carrier_operators(patch_baseline_[134])'s mask
    // (i.e. every modulator, for whichever of the 32 algorithms is
    // currently loaded) -- carriers are untouched, since boosting a
    // carrier only changes volume, not timbre.
    void  SetBrightness01(float v01);
    float GetBrightness01() const { return brightness01_; }
    // Scales all 6 operators' 4 EG rates together -- does NOT touch the
    // pitch EG (Dx7Note::update() never re-applies patch bytes 126-133
    // regardless, see dexed_synth.cpp's own comment).
    void  SetEnvSpeed01(float v01);
    float GetEnvSpeed01() const { return env_speed01_; }

    // Direct single-byte patch edit -- writes both patch_ and
    // patch_baseline_ (so Brightness/EnvSpeed's own baseline stays
    // consistent with whatever's actually playing) and pushes the
    // change to any currently-held voice via Dx7Note::update(), same
    // click-free mechanism as SetPatch(). byte_index is a raw offset
    // into the 156-byte unpacked layout (see dexed_synth.cpp's own
    // comment for the full byte map) -- deliberately low-level rather
    // than one named setter per parameter, since the UI needs this same
    // shape for several independent single-byte fields (algorithm,
    // feedback, LFO speed/depth) and a named setter per one would just
    // be boilerplate.
    void    SetPatchByte(int byte_index, uint8_t value);
    uint8_t GetPatchByte(int byte_index) const { return patch_[byte_index]; }

    // A flat POD snapshot of the currently-loaded sound -- everything
    // needed to reproduce it via ApplyPreset(), same "preset != session
    // mix" split GranularEngine::GranularPresetData/the removed
    // FmSynth::FmPresetData already use in this project.
    struct DexedPresetData
    {
        uint8_t patch[156]  = {};
        float   reverb_send01  = 0.2f;
        // 0.70 (not 1.0) so a freshly-loaded factory/new preset lands at a
        // safe, reasonable level by default -- factory presets never set
        // this themselves (GetFactoryPreset() only fills in patch[]), so
        // this default is literally what every one of the 700+ factory
        // presets loads at.
        float   output_level01 = 0.70f;
        int32_t filter_mode     = (int32_t)FilterMode::Off;
        float   filter_cutoff01 = 1.f;
        float   filter_res01    = 0.f;
        // Added once Dexed got a real Pan control -- 0.5 (centered), so
        // every DexedPresetData written before this field existed (a
        // positional/aggregate initializer that only lists the fields
        // that existed at the time) still gets a sensible neutral value
        // via this trailing default, same convention PadSynth's own
        // tune01/pan01 fields used when they were added later.
        float   pan01 = 0.5f;
        // Added once the shared delay bus existed -- same trailing-field/
        // 0.0-default convention pan01 above used, so every
        // DexedPresetData written before this field existed still gets a
        // sensible "no delay" default.
        float   delay_send01 = 0.f;
        // Added once the Dexed-only wavefolder existed -- same trailing-
        // field/safe-default convention pan01/delay_send01 above used.
        // 0.f = bypass (matches gain=1.0 unity default), 1.f = trim
        // unity (no change).
        float   fold_amount01 = 0.f;
        float   fold_trim01   = 1.f;
        // Added with the Mix page's Gain -- same trailing-field
        // convention again; 0 = 0 dB, so older presets sound unchanged.
        float   gain01 = 0.f;
        // Added in preset file v5. -1 = not stored (factory patches and
        // older files): loading leaves the current target alone.
        int32_t mod_wheel_target = -1;
        // The Ui's own Dexed MIDI channel (-1 = Omni), filled in by the
        // Ui on a user save -- CapturePreset() never sets it. -2 = not
        // stored: loading leaves the current channel alone.
        int32_t midi_channel = -2;
        // Added in preset file v6 -- the DX7 front panel's OP ON/OFF
        // switches (bit n = operator array index n, i.e. HW OP 6-n).
        // -1 = not stored (factory/SysEx patches and older files):
        // loading turns all six operators on, same as a real DX7.
        int32_t op_switch = -1;
        // Added in preset file v7 -- the MIDI page's MAX VOICES
        // (1..kMaxVoices). -1 = not stored (factory/SysEx patches and
        // older files): loading leaves the current limit alone.
        int32_t max_voices = -1;
    };
    // apply_bus = false loads only this engine's part (patch, OP
    // switches, mod wheel target, Voices) and leaves the shared bus's
    // Mix/FX/Filter/Fold alone -- used while the other engine is also on,
    // so picking a sound for B doesn't reset what A is playing through.
    void ApplyPreset(const DexedPresetData& p, bool apply_bus = true)
    {
        SetOpSwitch(p.op_switch >= 0 ? (uint8_t)(p.op_switch & 0x3f) : 0x3f);
        SetPatch(p.patch);
        if(p.mod_wheel_target >= 0)
            SetModWheelTarget((ModWheelTarget)p.mod_wheel_target);
        if(p.max_voices >= 1)
            SetMaxVoices(p.max_voices);
        if(!apply_bus)
            return;
        SetReverbSend01(p.reverb_send01);
        SetOutputLevel01(p.output_level01);
        SetFilterMode((FilterMode)p.filter_mode);
        SetFilterCutoff01(p.filter_cutoff01);
        SetFilterResonance01(p.filter_res01);
        SetPan01(p.pan01);
        SetDelaySend01(p.delay_send01);
        SetFoldAmount01(p.fold_amount01);
        SetFoldTrim01(p.fold_trim01);
        SetGain01(p.gain01);
    }
    DexedPresetData CapturePreset() const;

    // OP ON/OFF -- bit n = operator array index n (HW OP 6-n), set =
    // sounding. A muted operator's envelope still runs (see
    // Dx7Note::compute()), so turning it back on mid-note is seamless.
    void    SetOpSwitch(uint8_t mask) { ctrls_.opSwitch = mask & 0x3f; }
    uint8_t GetOpSwitch() const { return ctrls_.opSwitch; }

    // How many of the kMaxVoices slots new notes may use (1..kMaxVoices),
    // set from the MIDI page to trade polyphony for CPU when other
    // engines are busy. Lowering it releases any held notes above the
    // new limit, so they fade out on their own release.
    void SetMaxVoices(int n);
    int  GetMaxVoices() const { return max_voices_; }

    // The DX7's own INIT VOICE: algorithm 1, OP1 alone at level 99, a
    // plain sine with an organ envelope -- the standard blank starting
    // point for building a sound. Every other field keeps its default.
    static DexedPresetData GetInitVoice();

    // Factory presets: real DX7 patches, embedded from freely-
    // distributed real SysEx bank data (see dexed_factory_data.h/.cpp's
    // own doc comment for the source) -- organized into named
    // categories by real sound type (Synth/Piano/E.Piano/Bass/Strings/
    // Woodwind/Brass/Organ/Percussion/Choir/Bells), addressed by a flat
    // 0-based index resolved against that category table. Same
    // {name,count}-contiguous-range shape the removed FmSynth used for
    // its own (much smaller) factory bank, generalized to more/larger
    // categories. All `static` -- no instance needed, matching FmSynth's
    // own convention (PerformanceStore calls these directly).
    static int             GetNumFactoryPresets();
    static const char*     GetFactoryCategoryName(int cat);
    static int              GetFactoryCategoryCount(int cat);
    static int              GetFactoryCategorySlot(int cat, int local_index); // -> flat 1-based slot
    static DexedPresetData  GetFactoryPreset(int flat_index); // 0-based; unpacks on demand
    static const char*      GetFactoryPresetName(int flat_index); // reads the patch's own real name bytes
    static constexpr int kNumFactoryCategories = kDexedNumFactoryCategories;
    static constexpr int kNumRomCategories     = kDexedNumRomCategories;

    // Reads the same 10-character voice name (patch bytes 145-154, see
    // dexed_sysex.h's own layout comment) out of a DexedPresetData that's
    // already been decoded and is sitting in memory -- unlike
    // GetFactoryPresetName() (which unpacks a factory bank's own packed
    // bytes on demand), this never touches the SD card or the factory
    // banks: it's for USER slots, whose DexedPresetData already carries
    // its real name (a manual "Save New" keeps whatever name was in the
    // live patch_ at save time; a SysEx import keeps the bank's own
    // original name verbatim) even though the preset browser never used
    // to display it. Trimmed the same way (trailing spaces stripped),
    // returns "?" for an all-blank/default name so an empty result is
    // never mistaken for a real one. Own static buffer, separate from
    // GetFactoryPresetName()'s, so both can be used in the same
    // expression safely.
    static const char* GetPresetDataName(const DexedPresetData& p);

    // The voice slots allocated. 16 held notes measured ~27% on the CPU
    // meter with Dexed alone; was 10. Briefly dropped to 4 as a static,
    // always-on cap for a full chord + Grains hitting 100% -- reverted,
    // since a flat cap also limits Dexed-alone polyphony. The runtime
    // answer is max_voices_ (MIDI page K2), which the player lowers
    // when other engines need the CPU.
    static constexpr int kMaxVoices = 16;
    // Slots that never take a new note: they only hold a stolen voice
    // while it fades (see NoteOn()), so even 16 held notes plus an arm
    // across the keys has somewhere to fade up to 4 steals at once.
    static constexpr int kFadeSlots  = 4;
    static constexpr int kVoiceSlots = kMaxVoices + kFadeSlots;

  private:
    void RenderQuantum(); // fills stage_f_ with one fresh 64-sample msfa quantum

    // Real oldest-note-steal allocation -- see NoteOn()'s own doc
    // comment for the 3-tier shape (retrigger / free / steal).
    // *same_key: a voice still sounding this key (held, under the pedal or
    // releasing) -- released if held, and the new voice starts in phase
    // with it (see NoteOn()); -1 if none.
    int  FindVoiceForNote(uint8_t note, int* same_key);
    // True once every operator's envelope has fully reached its final
    // rest level (VoiceStatus::ampStep -- see msfa/env.cpp's own ix_
    // semantics: ix_ >= 4 means the envelope is no longer advancing at
    // all), i.e. genuinely safe to reassign with no audible cut -- vs.
    // merely "released" (NoteOff already called, but still ringing out).
    bool VoiceIsIdle(int voice_index);

    // Copies new_patch into patch_ and patch_baseline_ (the reference
    // point SetBrightness01()/SetEnvSpeed01() scale relative to,
    // re-centering both macros back to 0.5), then pushes it to every
    // currently-held voice via Dx7Note::update() (not init()) so
    // switching/previewing a preset doesn't retrigger or click an
    // already-held note. Used by ApplyPreset() and (in a later phase)
    // the Preset page's load/preview flow.
    void SetPatch(const uint8_t new_patch[156]);
    // Shared by SetPatch()/SetBrightness01()/SetEnvSpeed01() -- pushes
    // the current patch_ to every voice with held_note != -1 via
    // Dx7Note::update(), using that voice's own cached velocity.
    void ApplyPatchToHeldVoices();
    // `value` held to the range the engine can use for that byte (see
    // dexed_synth.cpp) -- applied to every patch on its way in.
    static uint8_t SafePatchByte(int byte_index, uint8_t value);
    // A key with TRANSPOSE (patch byte 144) applied, clamped to MIDI range.
    int  TransposedNote(int note) const;

    struct Voice
    {
        Dx7Note note;
        // -1 = free (eligible for reuse -- see NoteOff(), which frees a
        // voice immediately rather than waiting for its release tail to
        // finish). Otherwise the MIDI note currently assigned here.
        int      held_note   = -1;
        // The key this voice last started (kept through its release), so
        // striking that key again finds it -- see FindVoiceForNote().
        int      sounding_note = -1;
        // Cached at NoteOn() time -- Dx7Note::update() (unlike init())
        // still needs a velocity to recompute ScaleVelocity()-based
        // output level, but doesn't take one as an already-tracked
        // implicit voice property the way real hardware would.
        uint8_t  velocity     = 100;
        uint32_t triggered_at = 0;
        // Set by the first real NoteOn(). Before that a voice holds only
        // Init()'s silent pre-warm patch, whose envelope sits at sustain
        // forever -- so VoiceIsIdle() counts an unplayed voice as idle,
        // or every voice would be rendered (as silence) until first used.
        bool     played       = false;
        // Samples left in a steal fade (0 = not fading). A voice that is
        // still sounding when a new note needs its slot is copied here
        // first and faded out over kStealFadeSamples, so the restart
        // doesn't cut it off in a single sample (a click). See NoteOn().
        int      fade_left    = 0;
    };
    // ~5 ms at 48 kHz: long enough to hide the cut, short enough that
    // the copy never really adds a voice's worth of CPU.
    static constexpr int   kStealFadeSamples = 240;
    static constexpr float kStealFadeInv     = 1.f / (float)kStealFadeSamples;
    Voice       voices_[kVoiceSlots];
    // Power-on default: 12 held notes keeps Dexed around 20% CPU; the
    // player can take it up to kMaxVoices. See SetMaxVoices().
    int         max_voices_ = 12;
    // Monotonic, so "oldest" is always a plain min() over this -- same
    // idiom the removed FmSynth used for its own oldest-note-steal.
    uint32_t    trigger_seq_ = 0;
    Controllers ctrls_;
    EngineMsfa  engine_;
    // ONE shared LFO, not per-voice -- matches real DX7 hardware (the
    // LFO is global across every simultaneously-sounding voice, not an
    // independent oscillator per note).
    Lfo     lfo_;
    uint8_t patch_[156];
    // Pristine copy of patch_ as of the last SetPatch() call -- the
    // reference point Brightness/EnvSpeed scale relative to (so
    // repeatedly nudging a macro knob back and forth doesn't compound
    // rounding drift against its own previous output).
    uint8_t patch_baseline_[156] = {};
    // Centered at 0.5 ("as stored") -- see SetBrightness01()/
    // SetEnvSpeed01()'s own doc comment for the bipolar scale shape.
    float brightness01_ = 0.5f;
    float env_speed01_  = 0.5f;

    float sample_rate_     = 48000.f;
    ModWheelTarget mod_wheel_target_ = ModWheelTarget::Pitch;
    void           ApplyModWheelTarget();

    DexedBus* bus_ = nullptr;
    // See SetEngineLevel01()/SetEnginePan01(). Unity and centre: a lone
    // engine at its defaults feeds the bus exactly what the old single
    // Dexed fed its output stage.
    float engine_level01_ = 1.f;
    float engine_level_   = 1.f; // powf(engine_level01_, 2.5f), cached by the setter
    float engine_pan01_   = 0.5f;
    float bal_l_          = 1.f; // balance gains, cached by SetEnginePan01()
    float bal_r_          = 1.f;

    // msfa's native quantum -- Dx7Note::compute()/EngineMsfa::render()
    // always produce exactly this many samples per call (hardcoded via
    // _N_ throughout fm_op_kernel.cpp/env.cpp, see msfa/synth.h).
    static constexpr int kMsfaBlock = 64;
    // Each voice renders into this SAME scratch buffer in turn (reused,
    // not per-voice state -- cleared fresh before every voice), then
    // RenderQuantum() clamps and converts that one voice's own raw
    // output before summing it into stage_f_ below. Reference
    // implementation (positionhigh/MicroDexed-touch's Synth_Dexed,
    // dexed.cpp's getSamples()) does the same thing: per-voice scratch
    // buffer, per-voice saturate, sum in float -- NOT what this engine
    // originally did (every voice's compute() call sharing one int32
    // accumulator, meaning multiple voices added together in raw
    // int32_t arithmetic with no clamp at all, before this fix). That
    // mattered even for a single voice, not just chords: this engine's
    // own FM math can transiently produce a raw value outside its
    // nominal range (expected/normal -- it's exactly what the
    // reference's own clamp exists to catch), and reading that back out
    // further downstream can't distinguish a wrapped-but-still-valid-
    // looking int32 from a real one, so the clamp has to happen here,
    // right at the source, not later.
    int32_t voice_scratch_[kMsfaBlock];
    // The actual accumulator every voice's own clamped contribution
    // sums into -- float, not int32_t, specifically so summing several
    // voices can never itself overflow/wrap (floats saturate towards
    // +-inf instead, and kHeadroomScale/the tanhf() limiter downstream
    // already assume they might see a generously large value).
    float stage_f_[kMsfaBlock];
    // Index of the next not-yet-drained sample in stage_f_ -- starts at
    // kMsfaBlock so the very first Process() call renders a fresh
    // quantum immediately rather than reading uninitialized data.
    int stage_pos_ = kMsfaBlock;
};
