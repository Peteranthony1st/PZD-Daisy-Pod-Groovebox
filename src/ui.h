#pragma once
#include "daisy_pod.h"
#include "dev/oled_ssd130x.h"
#include "tempo_clock.h"
#include "looper_layer.h"
#include "performance_store.h"
#include "font_tomthumb.h"

class GranularEngine;
class DexedSynth;
class EnsembleSynth;
class SampleSynth;

// The whole "one encoder + push button, two knobs, two buttons, one small
// OLED" menu system.
//
// IMPORTANT real-time rule: Ui::Update() talks to the OLED over I2C using
// *blocking* transfers (that's how libDaisy's SSD130x I2C transport
// works -- see dev/oled_ssd130x.h). A full-screen redraw takes on the
// order of a few milliseconds. That is completely fine to call from
// main()'s while(1) loop, and absolutely NOT fine to call from the audio
// callback (it would starve the real-time audio budget and you'd hear
// clicks/dropouts). So:
//
//   - main.cpp's AudioCallback() only touches TempoClock::Process() and
//     LooperLayer::Process() -- pure per-sample DSP, no hardware polling.
//   - main()'s while(1) loop calls pod.ProcessAllControls() and
//     Ui::Update() at a modest, steady rate (see main.cpp). This is where
//     encoder/button/knob reading AND the OLED redraw happen.
//
// The two sides only ever touch plain float/enum fields on TempoClock and
// LooperLayer (SetVolume01, SetBpm, etc.) -- no locks are used, same as
// the original firmware's approach to knobs/switches. That's fine for
// this kind of "slow" control data; it would not be fine for anything
// requiring sample-accurate consistency.
//
// Encoder/button EDGE events (as opposed to level state like Pressed())
// come in via UiControlEvents rather than being read straight off
// daisy::Encoder/Switch here. Reason: Draw()'s blocking OLED I2C writes
// can stall the main loop for tens of ms, and Encoder::Debounce() only
// resolves ONE quadrature step per call (plus self-limits to 1kHz) --
// Switch::RisingEdge()/FallingEdge() are similarly a single-tick pulse.
// If those were serviced only when the (potentially stalled) main loop
// got around to it, a fast encoder turn or a quick tap landing during a
// redraw would be silently dropped, not just delayed. main.cpp instead
// drives Debounce() from a 1kHz timer ISR and accumulates ticks/edges
// into monotonic counters; UiControlEvents is the delta-since-last-call
// computed from those. See main.cpp's ControlTimerCallback().
struct UiControlEvents
{
    int32_t encoder_delta      = 0; // net encoder steps since the last Update()
    bool    encoder_click_fell = false; // encoder switch released since the last Update()
    bool    btn1_released      = false; // button1 released since the last Update()
    bool    btn2_released      = false; // button2 released since the last Update()
    // How long button1/2 was held for, valid whenever btn1_released/
    // btn2_released is true -- captured inside ControlTimerCallback()'s
    // own 1kHz ISR (see its comment), NOT read from Switch::TimeHeldMs()
    // here in the main loop. TimeHeldMs() itself reads 0 the instant
    // Pressed() goes false, i.e. the exact same tick a release is
    // detected -- if a press/hold/release cycle completes while the main
    // loop happens to be busy elsewhere (a slow OLED redraw, a blocking
    // SD read, or just an unlucky scheduling gap), it never gets a
    // chance to observe "still held past threshold" while the button was
    // down, and by the time it checks again TimeHeldMs() already reads 0
    // -- silently misclassifying a long hold as a short tap. These
    // fields exist so every long-press site can fall back to a value
    // that's immune to that race (see HandleButton1()/HandleButton2()).
    float   btn1_held_ms       = 0.f;
    float   btn2_held_ms       = 0.f;
};

class Ui
{
  public:
    void Init(daisy::DaisyPod* pod,
              daisy::OneBitGraphicsDisplay* display,
              TempoClock*                   tempo,
              LooperLayer*                  layers,
              int                           num_layers,
              GranularEngine*               granular,
              const float*                  granular_scope_buf,
              size_t                        granular_scope_capacity,
              float*                        granular_capture_buf_l,
              float*                        granular_capture_buf_r,
              size_t                        granular_capture_capacity,
              volatile bool*                granular_capturing,
              volatile size_t*              granular_capture_write_pos,
              const float*                  master_scope_buf,
              size_t                        master_scope_capacity,
              const float*                  dexed_scope_buf,
              size_t                        dexed_scope_capacity,
              DexedSynth*                   dexed,
              EnsembleSynth*                ensemble,
              const float*                  ensemble_scope_buf,
              size_t                        ensemble_scope_capacity,
              SampleSynth*                  sample_synth,
              const float*                  sample_synth_scope_buf,
              size_t                        sample_synth_scope_capacity,
              const float*                  bypass_scope_buf,
              size_t                        bypass_scope_capacity,
              float*                        sample_synth_arena_l,
              float*                        sample_synth_arena_r,
              size_t                        sample_synth_arena_capacity,
              float*                        sample_synth_capture_buf_l,
              float*                        sample_synth_capture_buf_r,
              size_t                        sample_synth_capture_capacity,
              volatile bool*                sample_synth_capturing,
              volatile size_t*              sample_synth_capture_write_pos,
              float*                        undo_backup_l,
              float*                        undo_backup_r,
              int*                          undo_owner_layer,
              size_t*                       undo_backup_len);

    // Call once from main(), right after Init() -- applies the user's
    // saved startup defaults (see PerformanceStore::LoadPrefs()) on top
    // of Init()'s own hardcoded ones. A no-op if nothing's been saved
    // yet (no SD card, or the user's never used Global:File's Button2
    // tap to save one).
    void ApplyStartupDefaults();

    // Call once from main(), after both this Init() and the PWM
    // peripheral itself are set up -- see main.cpp's led_pwm_tim3 for
    // why only these 3 of the Pod's 6 RGB sub-pixels get true hardware
    // PWM. UpdateLeds() (private, below) calls each channel's own Set()
    // directly instead of the old pod_->led1/led2.Set() path.
    void SetLedPwmChannels(daisy::PWMHandle::Channel* led1_green,
                            daisy::PWMHandle::Channel* led1_blue,
                            daisy::PWMHandle::Channel* led2_red)
    {
        led1_green_pwm_ = led1_green;
        led1_blue_pwm_  = led1_blue;
        led2_red_pwm_   = led2_red;
    }

    // Call once per main-loop iteration (NOT from the audio callback).
    void Update(const UiControlEvents& events);

    // Read by main.cpp's audio callback every block.
    float GetMasterVolume() const { return master_volume_; }
    bool  IsBypassed() const { return bypass_; }
    // Master-bus filter, applied once to the full mix (all layers plus
    // their reverb) rather than per-layer -- see main.cpp's
    // fx_master_filter_l/r. Cutoff/Resonance are raw 0..1; main.cpp
    // converts them using the same curve LooperLayer's per-layer filter
    // uses (kFilterMinHz/kFilterMaxHz in looper_layer.h).
    FilterMode GetMasterFilterMode() const { return master_filter_mode_; }
    float GetMasterFilterCutoff01() const { return master_filter_cutoff01_; }
    float GetMasterFilterResonance01() const { return master_filter_res01_; }
    // Shared reverb bus's Size/decay (see main.cpp's fx_reverb_shared) --
    // every layer's independent Send feeds this ONE instance now, not a
    // per-layer ReverbSc any more (see LooperLayer::SetReverbSend01()'s
    // comment for why). Raw 0..1, applied via SetFeedback() directly
    // (same range DaisySP's ReverbSc expects), no curve needed.
    float GetReverbSize01() const { return reverb_size01_; }
    // The other 6 DattorroReverb parameters -- Size/Decay above is the
    // one that was already live; these back Global:Reverb Tone/Diffusion
    // (see GlobalPage::ReverbTone/ReverbDiffusion). Raw 0..1, applied via
    // the matching DattorroReverb setter directly, no curve.
    float GetReverbPreDelay01() const { return reverb_predelay01_; }
    float GetReverbPreFilter01() const { return reverb_prefilter01_; }
    float GetReverbInputDiffusion1_01() const { return reverb_input_diffusion1_01_; }
    float GetReverbInputDiffusion2_01() const { return reverb_input_diffusion2_01_; }
    float GetReverbDecayDiffusion01() const { return reverb_decay_diffusion01_; }
    float GetReverbDamping01() const { return reverb_damping01_; }
    // Shared delay bus's Time/Feedback (see main.cpp's fx_delay_l/r) --
    // same "one shared instance, everyone sends into it" relationship to
    // Dexed's/Grains' own Delay Send as Reverb Size has to their Reverb
    // Send. Raw 0..1 -- main.cpp maps Time onto an actual ms value with
    // its own exponential curve, and scales Feedback below 1.0 to avoid
    // runaway self-oscillation in the cross-feedback pair.
    float GetDelayTime01() const { return delay_time01_; }
    float GetDelayFeedback01() const { return delay_feedback01_; }
    // Everything the shared delay bus needs, for main.cpp and WAV export.
    SharedDelay::Settings GetDelaySettings() const
    {
        SharedDelay::Settings s;
        s.time01     = delay_time01_;
        s.feedback01 = delay_feedback01_;
        s.sync       = delay_sync_;
        s.pingpong01 = delay_pingpong01_;
        s.tone01     = delay_tone01_;
        return s;
    }
    float GetDelayToReverb01() const { return delay_to_reverb01_; }
    // The same, plus Bypass's Delay Send, as saved in performances and
    // startup defaults (file version 16+).
    PerformanceStore::DelayState GetDelayState() const
    {
        PerformanceStore::DelayState d;
        d.settings      = GetDelaySettings();
        d.to_reverb01   = delay_to_reverb01_;
        d.bypass_send01 = bypass_delay_send01_;
        return d;
    }
    void ApplyDelayState(const PerformanceStore::DelayState& d)
    {
        delay_time01_        = Clamp01(d.settings.time01);
        delay_feedback01_    = Clamp01(d.settings.feedback01);
        delay_sync_          = d.settings.sync;
        delay_pingpong01_    = Clamp01(d.settings.pingpong01);
        delay_tone01_        = Clamp01(d.settings.tone01);
        delay_to_reverb01_   = Clamp01(d.to_reverb01);
        bypass_delay_send01_ = Clamp01(d.bypass_send01);
    }
    static float Clamp01(float v) { return v < 0.f ? 0.f : v > 1.f ? 1.f : v; }
    // Which MIDI channel each engine responds to -- -1 = Omni (respond to
    // everything, the default, matching this project's own behavior
    // before per-engine routing existed). 0-15 = a specific channel
    // (0-indexed, matching libDaisy's own MidiEvent::channel convention
    // -- "channel 5" is stored/compared as 4). Read once per incoming
    // MIDI event in main.cpp's MIDI block, not per audio sample.
    int GetDexedMidiChannel() const { return dexed_midi_channel_; }
    int GetGranularMidiChannel() const { return granular_midi_channel_; }
    int GetEnsembleMidiChannel() const { return ensemble_midi_channel_; }
    int GetSampleSynthMidiChannel() const { return sample_synth_midi_channel_; }
    // How much of the Bypass live-monitor signal feeds the shared reverb
    // bus above -- independent of every layer's own Send, same shared
    // bus though (see main.cpp's AudioCallback()). 0 (default) = none,
    // matching every per-layer Send's own default.
    float GetBypassReverbSend01() const { return bypass_reverb_send01_; }
    // Same, into the shared delay bus (Global:Bypass, B2).
    float GetBypassDelaySend01() const { return bypass_delay_send01_; }
    // Shared delay Time for the screen: "Off" at 0, otherwise ms, or the
    // note value (e.g. "1/8D") with Sync on.
    void FormatDelayTime(float time01, char* out, size_t out_size) const;
    // Gain applied to the Bypass dry monitor mix -- uses whichever layer
    // is currently selected on the Home screen, since Bypass is mainly
    // used to check levels right before recording into that layer (see
    // LooperLayer::SetInputGain01(), the same control used at actual
    // record time).
    float GetBypassGain() const { return layers_[cursor_layer_].GetInputGain(); }
    // Screen::Mixer's own Bypass channel -- a genuine mix-level fader
    // multiplied ON TOP of GetBypassGain() above, not a replacement for
    // it: GetBypassGain() is deliberately about gain-staging/checking
    // levels before recording (see its own comment), while this is about
    // how loud Bypass sits in the overall final mix once you're past
    // that -- two different jobs on the same live signal, so this is
    // additive rather than reusing/repurposing the existing knob.
    // Defaults to 0.8f (matching every other engine's own Output Level
    // default in this codebase) since this is a brand new control with
    // no prior loudness to preserve -- same curve as every other Volume-
    // type knob (LooperLayer::SetVolume01(), GranularEngine's own
    // SetOutputLevel01()).
    float GetBypassMixVolume01() const { return bypass_mix_volume01_; }
    float GetBypassMixVolume() const { return bypass_mix_volume_; }
    // Defined in ui.cpp, not inline here -- needs powf(), same reasoning
    // every other curved setter in this header already avoids inlining.
    void  SetBypassMixVolume01(float v01);
    // Linear pan law, same as every other engine's own Pan (see
    // GranularEngine::SetPan01()'s comment) -- main.cpp computes panL/panR
    // from this directly (0.5 = center), same "no cached gain members
    // needed, this is UI-owned global state not a DSP object" treatment
    // as GetMasterVolume()'s own curve being applied inline there.
    float GetBypassPan01() const { return bypass_pan01_; }
    void  SetBypassPan01(float v01)
    {
        bypass_pan01_ = v01 < 0.f ? 0.f : (v01 > 1.f ? 1.f : v01);
    }
    // Block-rate: tape-style multiplier applied on top of every layer's
    // own Speed and TempoClock's own tick rate -- see main.cpp's
    // AudioCallback(), TempoClock::Process(), LooperLayer::Process().
    // 1.0 (default) = normal.
    float GetProjectSpeed() const { return project_speed_; }

    // Per-engine on/off (Global:Granular, Button1 tap) -- pulled forward
    // from the granular-rewrite plan's Stage 4 once real hardware
    // measurement showed CPU headroom was tight enough that a real way
    // to free up budget was needed now, not just once Granular's own UI/
    // persistence stages were done. main.cpp's AudioCallback() skips a
    // disabled engine's Process() call entirely (writing silence
    // instead) -- a real CPU saving, not just a mute.
    bool IsGranularEnabled() const { return granular_enabled_; }
    // Same real-CPU-saving on/off gate as Granular's own, for the new
    // Dexed engine (see main.cpp's AudioCallback()) -- no mutex with any
    // other instrument needed (unlike the old Pad/Fm pairing), Dexed is
    // independent of Granular the same way Granular is independent of
    // the looper.
    bool IsDexedEnabled() const { return dexed_enabled_; }
    // Same real-CPU-saving on/off gate, for the new Ensemble engine (see
    // main.cpp's AudioCallback()).
    bool IsEnsembleEnabled() const { return ensemble_enabled_; }
    // Same real-CPU-saving on/off gate, for the shared SampleSynth
    // engine -- true (and Process() runs) whenever EITHER SMPVO or
    // SMPDR is enabled (see sample_synth_voice_enabled_'s own comment
    // for the mutual-exclusion interlock between the two).
    bool IsSampleSynthEnabled() const
    {
        return sample_synth_voice_enabled_ || sample_synth_drum_enabled_;
    }
    // Global:Looper, Button1 tap -- a REAL stop, not a layer's own
    // phase-locked pause: main.cpp's AudioCallback() skips every
    // LooperLayer::Process() call entirely while this is false, so
    // someone using only Dexed/Grains gets that CPU back rather than 4
    // idle-but-still-processing layers. Defaults true (unlike Pad/
    // Granular's own false default) since the loop is this project's
    // original core feature, not an add-on someone opts into.
    bool IsLooperEnabled() const { return looper_enabled_; }

  private:
    enum class Screen
    {
        Home,
        Layer,
        Global,
        // A real top-level screen, not a Global page -- entered from
        // Global's own Granular entry-point page (encoder click), exits
        // back to Home via the same long-press-from-any-non-Home-screen
        // path every other screen uses. See GranularParamPage for its
        // own pages.
        Granular,
        // Same treatment as Granular above, entered from Global:Mixer.
        // Unlike every other screen, encoder rotate here doesn't select a
        // page -- it scrolls through one continuous, endlessly-wrapping
        // sequence of "stops" (see mixer_position_): the 8 real channels
        // (each showing a Detail page with real Vol/Pan/Rev bars for the
        // group of 4 it belongs to), then a Scope stop (the final post-
        // fader mix), then back to channel 0 -- Global:Mixer's own page
        // already covers the "all 8 at a glance" job, so there's no
        // separate Overview stop in here too. Encoder click is unused
        // here, same as every other Pad/Granular-style screen.
        Mixer,
        // Same treatment as Granular above, entered from Global:Dexed.
        // See DexedParamPage for its own pages.
        Dexed,
        // Same treatment as Granular/Dexed above, entered from
        // Global:Ensemble. See EnsembleParamPage for its own pages.
        Ensemble,
        // Same treatment as Granular/Dexed/Ensemble above, entered from
        // Global:SampleSynth. See SampleSynthParamPage for its own pages.
        SampleSynth,
        // The advanced per-operator FM editor -- entered from
        // Screen::Dexed's own Advanced page (encoder click), NOT a
        // Global page of its own. Encoder click here returns to
        // Screen::Dexed (landing back on the Advanced page); long-press
        // still goes all the way back to Home like every other screen.
        // See DexedOpParamPage for its own pages.
        DexedOperator
    };
    // Shared save/load interaction state for Global:File, Pad Preset, and
    // Grains Preset -- only one of those three pages is ever visible at
    // once, so one set of fields covers all three (see save_load_mode_'s
    // own comment for the reset-on-leave rule that keeps that safe).
    // Idle: just shows current status, Button1/Button2 say "Save"/"Load".
    // ChoosingSave: Button1 was tapped -- Knob1 picks Overwrite (only
    // offered when something's actually loaded) vs Save New, Button1's
    // own label becomes "Hold=Save" to commit whichever's picked.
    // BrowsingLoad: Button2 was tapped -- Knob1 browses the slot list
    // (same as it already did unconditionally before this), Button2's
    // label becomes "Hold=Load" to commit.
    enum class SaveLoadMode
    {
        Idle,
        ChoosingSave,
        BrowsingLoad
    };
    // Global:Speed's own transport sub-mode, cycled by a Button1 short
    // tap. Normal: encoder rotation does nothing here. Scrub: encoder
    // rotation nudges every layer's play_pos_ together (see ScrubBy()),
    // normal tempo-driven playback keeps advancing underneath. Freeze:
    // encoder rotation moves a small frozen "drone" window instead (see
    // NudgeFreeze()) -- normal playback genuinely stops advancing while
    // active, see LooperLayer::SetFreezeActive()'s own doc comment.
    enum class SpeedTransportMode
    {
        Normal,
        Scrub,
        Freeze
    };
    enum class LayerPage
    {
        Status,
        // Recording input gain (SetInputGain01() -- boosts a quiet mic/
        // line source before capture), not a playback volume control.
        // Sits right after Status, before the playback-shaping pages
        // below (Speed/Filter/Effect/FX), since it only matters before
        // or during recording, not to what's already been captured.
        Gain,
        Speed,
        Filter,
        Effect,
        Reverb,
        kCount
    };
    enum class GlobalPage
    {
        Tempo,
        Speed, // Sits right after Tempo -- both are project-wide timing/transport controls
        Filter,
        Reverb,
        // Reverb's own two follow-on pages -- Decay/Size itself stays on
        // Reverb (GLB-FX) above, these cover the rest of the Dattorro
        // engine's parameters (see DattorroReverb). ReverbTone's own pair
        // is Button-toggled (PreDelay+PreFilter vs. DecayDiffusion+
        // Damping); ReverbDiffusion has only 2 knobs left (Input
        // Diffusion 1+2) so needs no toggle.
        ReverbTone,
        ReverbDiffusion,
        // How much of the shared delay's output also feeds the shared
        // reverb (K1), so echoes sit in the same space as the dry sound.
        DelayReverb,
        // The shared delay bus: K1 Time (ms, or a note value with Sync
        // on), K2 Feedback, B1 Sync on/off.
        Delay,
        // Delay character: K1 Ping-Pong, K2 Tone (darker repeats).
        DelayTone,
        // Bypass (live input monitor) sends into the shared busses --
        // same shape as every engine's own FX page: B1 = Reverb (K1
        // Send, K2 shared Size), B2 = Delay (K1 Send, K2 shared Time).
        Bypass,
        File,
        // File management: Knob1 picks which of the 2 save folders
        // (Performances/Grains Presets) then Button1 tap drills in; once
        // inside, Knob1 browses that folder's own files (reusing the
        // exact same list Global:File/Grains Preset already scan and
        // cache), Button1 tap goes back up, Button1 hold Duplicates the
        // browsed file, Button2 hold Deletes it. See
        // SdMgmtFolder/sd_mgmt_in_folder_.
        SdMgmt,
        Export,
        // Entry point into Screen::Granular (not built yet -- Stage 2 of
        // the granular rewrite). For now this page only exposes the
        // on/off toggle pulled forward from Stage 4, same idiom as Pad's
        // own toggle below.
        Granular,
        // Entry point into Screen::Dexed, plus the on/off toggle -- same
        // Button1-tap convention as Granular's own above.
        Dexed,
        // Entry point into Screen::Ensemble, plus the on/off toggle --
        // same Button1-tap convention as Granular/Dexed above.
        Ensemble,
        // Entry point into Screen::SampleSynth, plus the on/off toggle --
        // same treatment as Granular/Dexed/Ensemble above. Split into
        // two entries (Voice/Drum, see Ui::SampleSynthMode's own
        // comment) sharing that one Screen/engine underneath --
        // enabling one forces the other's own enabled flag off (see
        // sample_synth_voice_enabled_'s own comment), so at most one
        // is ever actually processing audio.
        SampleSynthVoice,
        SampleSynthDrum,
        // On/off toggle only, same treatment as Granular/Dexed above --
        // but for the whole 4-layer loop system, and a REAL stop (see
        // IsLooperEnabled()'s doc comment), not a layer's own
        // phase-locked pause. Lets someone using only Dexed/Grains skip
        // all 4 LooperLayer::Process() calls entirely for the CPU back,
        // not just silence.
        Looper,
        // Entry point into Screen::Mixer -- no knobs of its own any more
        // (the real per-engine level controls live there), same "entry
        // point only" idiom as Granular/Dexed above.
        Mixer,
        kCount
    };
    // Declared in this exact order because DrawSdMgmtScreen()'s own grid
    // maps (int)SdMgmtFolder straight to a row-major cell index (4
    // columns x 2 rows) -- reordering these moves which box each one
    // draws in. Never persisted anywhere (sd_mgmt_folder_ always boots
    // back to Performances), so reordering is otherwise free.
    enum class SdMgmtFolder
    {
        Performances,     // row0 col0
        GranularPresets,  // row0 col1
        // User-saved Dexed presets only, same "factory bank is built
        // into firmware, nothing to delete there" split GranularPresets
        // (whose own PRES001-009 starter presets ARE real deletable
        // files, unlike Dexed's) already documents below -- reuses
        // RefreshDexedPresetSlots()/dexed_preset_user_slots_
        // DexedParamPage::Preset already maintains. Note this excludes
        // any preset inside a named SysEx-import folder, same as that
        // page's own User group -- see dexed_preset_user_slots_'s own
        // comment.
        DexedPresets,     // row0 col2
        // User-saved Ensemble presets only -- the factory bank isn't
        // real files (nothing to delete), same "factory + user, only
        // user is manageable here" split GranularPresets/Dexed's own
        // presets use, reusing the same RefreshEnsemblePresetSlots()/
        // ensemble_preset_user_slots_ EnsembleParamPage::Preset already
        // maintains.
        EnsemblePresets,  // row0 col3
        // SampleSynth's own Captures (SampleSynthParamPage::Trim's B2
        // hold-to-save, see TriggerSaveSampleSynthCapture()) -- NOT
        // Kits, NOT ordinary Sounds, just the CAPTUREDn bank files.
        // Browsed bank first, then file -- see SdMgmtIsBankFolder().
        SampleSynthCaptures,       // row1 col0
        // SMPIMP: everything imported into the Sampler -- Voice -> the
        // SOUNDSn banks' sound files and MULTISMPn multi-sample folders, Drum ->
        // Kits / Breaks -> the KITSn/BREAKSn banks' kit folders. Captures
        // stay in their own CAPT box.
        SampleSynthImports,        // row1 col1
        // SampleSynth's own SMPVO User presets (SAMPLES/VOICE PRESETS/)
        // -- deliberately its own box, not combined with Drum User the
        // way Kits combines KITSn/BREAKSn, since these two
        // are a different engine *mode* each, not two halves of one
        // thing.
        SampleSynthVoiceUserPresets, // row1 col2
        // SampleSynth's own SMPDR User presets (SAMPLES/DRUM PRESETS/).
        SampleSynthDrumUserPresets,  // row1 col3
        kCount
    };

    // Screen::Granular's own pages -- Capture/Preset follow once real
    // capture/persistence logic exists (Stage 3); everything else here
    // just needed knobs wired to GranularEngine methods already built in
    // Stage 1.
    enum class GranularParamPage
    {
        // The showcase page: full-sample waveform + live grain-anchor
        // markers, Size/Fill/Gap/Jitter values shown live -- Button1 maps
        // the knobs to Size+Fill, Button2 maps them to Gap+Jitter (see
        // granular_grain_target_gap_jitter_), same toggle idiom as Pad's
        // own merged ADSR page. Jitter lives here (not its own page) --
        // it's a Grain-layer-only refinement, same family as Size/Fill/
        // Gap, not a separate feature; see GranularEngine::SetJitter01()
        // for what it actually does (a fixed Position anchor replays the
        // exact same buffer offset every retrigger, so a source with a
        // strong feature sitting inside the grain's window -- not at its
        // very edge, where the Hann window would silence it -- repeats
        // it identically forever, audible as a perfectly periodic click;
        // Jitter smears consecutive grains across nearby offsets
        // instead -- confirmed via a real exported capture on a
        // sustained pad sample).
        Grain,
        // Knob1 = Position (Grain layer's fixed anchor), Knob2 =
        // Direction (moved here from TuneDirection, below -- that page's
        // K2 slot is now free since this one was, per a real user
        // request). Button1/2 cycle Rhythm/Speed. Sits right after Grain
        // so the Grain layer's own two pages (what plays, then where it
        // reads from) are adjacent.
        Position,
        // Knob1 = Scan (speed/direction, dead-zone-centered mute -- moved
        // here from the Grain page), Knob2 = Scan's own Fill (how many
        // concurrent grains the Scan layer uses, independent of the
        // Grain layer's own Fill -- see GranularEngine::SetScanFill01()).
        // Split into its own page specifically so these two live
        // together: Scan is a genuinely separate layer from Grain (its
        // own anchor, its own density), so it gets its own independent
        // density control instead of being forced to always match
        // whatever the Grain layer's Fill happens to be. Sits right
        // after Position and before ScanRange, below, so the Scan
        // layer's own two pages are adjacent to each other, mirroring
        // Grain/Position's own pairing.
        Scan,
        // Knob1 = Scan Start, Knob2 = Scan End -- the Scan layer's own
        // sweep range, fully adjustable at both ends (see
        // GranularEngine::SetScanEnd01()'s own doc comment). Sits right
        // after Scan specifically since it's that page's own direct
        // sibling -- Scan is "what plays" for that layer, this is "where
        // it reads from," same relationship Position has to Grain.
        // Previously Scan's end was fixed to the buffer's own end and
        // only the start (then called "Scan Position") lived on the
        // Position page's K2; moved out to its own page once End became
        // independently adjustable too, per a real user request.
        ScanRange,
        // Knob1 only = Tune (semitones); Button1 cycles Map-to-Note
        // on/off. Direction (previously this page's own K2) moved to
        // Position's own K2 -- see its own doc comment above.
        Tune,
        // Attack/Decay/Sustain/Release graph, same merged-page idiom as
        // Grain's Size/Fill/Gap/Scan and Pad's own ADSR page.
        ADSR,
        Filter, // Cutoff + Resonance, mode cycled by Button1; live oscilloscope
        Mix,    // Grain layer volume + Scan layer volume
        // Full reverb/delay control from within Grains itself -- same
        // shape/reasoning as DexedParamPage::FX.
        FX,
        // K1 only, same knob-quantized idiom as DexedParamPage::MIDI --
        // which incoming MIDI channel this engine responds to. Default
        // Omni.
        MIDI,
        // Button1 cycles the source (Direct Record / From the currently
        // selected loop layer); Button2 held either records live input
        // for as long as it's held (Direct) or, past an 800ms threshold,
        // fires an instant copy (From Layer) -- see
        // Ui::TriggerGranularCaptureFromLayer() and HandleButton2()'s own
        // handling of this page. No continuous knobs.
        Capture,
        // Knob1 = trim start, Knob2 = trim end -- operates on whatever
        // SetSource() currently points at (Direct Record, From Layer, or
        // a loaded Preset), not just Direct Record specifically. Shows
        // the FULL original capture's waveform (granular_trim_full_peaks_,
        // a separate cache from GranularEngine's own -- see its comment)
        // with the trim points as the two markers, reusing
        // DrawGranularWaveform() for that -- same drawing code, just
        // start/end instead of Grain/Scan anchors. Non-destructive: the
        // underlying buffer is untouched, only the sub-range passed to
        // SetSource() changes, so re-opening this page and trimming again
        // always works from the original full capture.
        Trim,
        // Load-only library, shared browse convention: a folder list
        // (Starter = GRAINS/PRESETS, then Captured 1..n = GRAINS/CAPTUREDn,
        // filled by Trim's own Save), K1 scrolls folders, K2 the sounds
        // inside one, a tap loads and stays. Loads stream the captured
        // audio too, with a live progress bar (Ui::OnSaveLoadProgress()).
        Preset,
        // Your own edited saves (GRAINS/PRESnnn.DAT): B1 saves (K1 picks
        // Overwrite/Save New, hold B2 confirms), B2 loads from a list
        // that starts with "New (blank)".
        User,
        kCount
    };

    // Screen::Dexed's own pages -- a deliberately small "simple tier"
    // (see the approved UI plan) for quick sound-shaping; deep per-
    // operator editing lives in a separate advanced editor (a later
    // phase), reached by an encoder click from the Preset page below.
    enum class DexedParamPage
    {
        Algo,       // Button1 cycles the 32 real DX7 algorithms; no knobs
        Feedback,   // K1 only, 0-7
        // K1 = LFO speed, K2 = LFO pitch mod depth -- the patch's own
        // baked-in automatic vibrato, distinct from the mod wheel's own
        // separate ctrls_.wheel routing (already wired in Phase 4).
        Vibrato,
        // Bipolar macro, K1 only -- see DexedSynth::SetBrightness01()'s
        // own doc comment for the scaling shape.
        Brightness,
        // Bipolar macro, K1 only -- see DexedSynth::SetEnvSpeed01()'s own
        // doc comment.
        EnvSpeed,
        Filter, // Cutoff + Resonance, mode cycled by Button1
        Mix,    // Reverb Send + Output Level
        // Full reverb/delay control from within Dexed itself: Button1/
        // Button2 toggle between Reverb (own Send + shared Size) and
        // Delay (own Send + shared Time) -- Mix's own Reverb Send/Output
        // Level are untouched (this is a separate, additional page, not
        // a replacement -- Reverb Send is genuinely reachable from both
        // places, same "same value, multiple entry points" idiom already
        // established for Reverb Send across Mix/Global Mixer).
        FX,
        // Dexed-only wavefolder. K1 = Fold Amount, K2 = output trim --
        // no Button1/Button2 toggle, unlike FX above, since folding
        // has no sub-mode (see DexedSynth::SetFoldAmount01()'s own
        // comment).
        Fold,
        // K1 only, knob-quantized across 17 buckets (Omni + channels
        // 1-16) -- which incoming MIDI channel this engine responds to.
        // Default Omni (respond to everything), same behavior as before
        // this existed -- see main.cpp's MIDI block for the actual filter.
        MIDI,
        // Entry point into Screen::DexedOperator (encoder click) -- no
        // continuous knobs of its own, same "entry point only" idiom as
        // GlobalPage::Granular/Dexed. Positioned right before Preset so
        // the deep-editing detour sits next to the save/load workflow
        // it's meant to feed into, without being the very last page.
        Advanced,
        // SysEx import: K2 scrolls the .syx files in DEXED IMPORT/, hold
        // B2 imports one (each becomes its own folder under the Preset
        // page's Imports group). Same place as the Sampler's own Import.
        Import,
        // Load-only browser, shared browse convention: groups (Roms /
        // Dexed / Imports) -> folders -> presets. K1 scrolls groups and
        // folders, K2 the presets in an open folder, a tap loads and stays.
        Preset,
        // Your own saves (+ New): B1 saves (K1 Overwrite/Save New, hold
        // B2), B2 loads from a list that starts with "New".
        User,
        kCount
    };

    // Screen::Ensemble's own pages -- same "simple tier" shape as
    // DexedParamPage/SampleSynthParamPage above, no separate advanced
    // editor (EnsembleSynth has no per-operator-equivalent depth to
    // warrant one).
    enum class EnsembleParamPage
    {
        // K1 selects which of the 7 real drawbars (0-6), K2 sets that
        // drawbar's level -- see EnsembleSynth::SetDrawbarLevel()'s own
        // doc comment.
        Registration,
        // K1/K2 = Attack/Decay or Sustain/Release, Button1/Button2
        // target toggle -- same shape as SampleSynthParamPage::Env.
        Env,
        // K1 = chorus depth, K2 = chorus rate -- the one shared Chorus
        // instance applied to the summed voice mix.
        Ensemble,
        Filter, // Cutoff + Resonance, mode cycled by Button1
        Mix,    // Output Level + Pan
        // Button1/Button2 toggle between Reverb Send and Delay Send,
        // same convention as DexedParamPage::FX/SampleSynthParamPage::FX.
        FX,
        // K1 only, knob-quantized across 17 buckets (Omni + channels
        // 1-16) -- same convention as DexedParamPage::MIDI.
        MIDI,
        // Save/Load -- same flat "Files (numbered list, factory presets
        // first, read-only, then User saves) / Load New" chooser shape as
        // GranularParamPage::Preset, just with the small bundled factory
        // bank prepended to the browsable list instead of a separate
        // category. Positioned last, same relative order as
        // DexedParamPage/GranularParamPage/SampleSynthParamPage's own
        // Preset.
        Preset, // the factory sounds -- load only
        User,   // your own saves (+ New): save, overwrite, load
        kCount
    };

    // Screen::SampleSynth's own pages -- no kit-load/assign page yet
    // (needs PerformanceStore's own kit-loading pipeline first), just
    // the controls SampleSynth already has: filter, per-voice ADSR, and
    // output mix/FX sends, each mirroring Dexed/Grains' own identical
    // pages.
    enum class SampleSynthParamPage
    {
        Filter, // Cutoff + Resonance, mode cycled by Button1 -- same shape as Dexed/Grains
        // K1/K2 = Attack/Decay (AD) or Sustain/Release (SR), Button1
        // targets AD, Button2 targets SR -- same explicit-set convention
        // DexedParamPage::FX's own Reverb/Delay toggle uses.
        Env,
        Mix, // Output Level + Pan
        // Mono/Choke toggle (Button1, moved here from Mix -- see
        // SampleSynthPresetData::mono_choke's own comment) plus two
        // controls for an already-loaded Kit/Sound that Capture/Trim/
        // Tune can't reach (that group only ever operates on a fresh
        // capture's slot 0, by design -- see Tune's own comment above).
        // K1 = Tune: -24..+24 semitones, live/instant, applied to
        // EVERY currently active slot via SetSlotPitchOffsetSemis() --
        // a flat additive shift that works the same for a chromatic
        // Sound (no need to cache/restore its original root_note) and
        // a Kit's one-shot pads (which otherwise always play at native
        // pitch regardless of key -- see that setter's own comment).
        // K2 = Slices: shows a
        // *pending* count only (0-16, pre-loaded to 16 if nothing's
        // configured yet, else whatever's already applied) -- Button2
        // held confirms it, re-chopping whatever's currently loaded
        // into that many equal-length one-shot pieces on consecutive
        // keys from C2 (0 = off, revert to the kit's real per-slice
        // layout). Both Tune and Slices are pure live/session state,
        // same as Mono/Choke/Pan/Level -- there's no user-facing Save
        // for a Kit at all (see Preset's own doc comment below), so
        // nothing persists them, and a fresh load always starts at
        // off/0. Available on every Kit, not just real OP-1 break
        // imports (no reliable way to tell them apart), and
        // deliberately independent of Mono/Choke (user's own call).
        Setup,
        // Reverb send / Delay send, Button1/Button2 toggle -- same shape
        // as DexedParamPage::FX.
        FX,
        // K1 only, same knob-quantized idiom as DexedParamPage::MIDI/
        // GranularParamPage::MIDI -- which incoming MIDI channel this
        // engine responds to. Default Omni. Sits right after FX, same
        // relative position Grains' own MIDI page has.
        MIDI,
        // Record live input, pull from a loop layer, or import a .wav --
        // same Direct Record/From Layer/Import source cycle as
        // GranularParamPage::Capture, writing into sample_synth_capture_
        // buf_l_/r_ instead of Grains' own buffer. Always lands in slot
        // 0 (see sample_synth_trim_start01_'s own comment) and replaces
        // whatever Sound/Kit was loaded -- every other slot gets
        // cleared, by explicit decision (this is a scratchpad for new
        // content, not a way to add one more layer to an existing kit).
        Capture,
        // K1/K2 = trim start/end, non-destructive (only which sub-range
        // of the capture buffer slot 0 points at changes -- see
        // sample_synth_trim_start01_'s own comment). Button1 = Play
        // (SampleSynth::PlayPreview(), plays exactly the trimmed range
        // once). Button2 held 800ms = save the trimmed range as a real
        // Sound file (TriggerSaveSampleSynthCapture()) -- unlike Grains,
        // which saves a whole self-contained preset on a separate page,
        // a captured Sound here becomes an ordinary file under
        // the lowest SAMPLES/CAPTUREDn with room, ready to browse/load
        // like anything else.
        Trim,
        // K1 = Tune, same +-24 semitone range/quantization and dead-
        // center-is-zero feel as GranularParamPage::Tune (0 = "play back
        // exactly as captured"), only audible once chromatic is on.
        // Button1 toggles chromatic on/off -- same meaning as Grains'
        // own Map-to-Note. Both map onto fields SampleSlot already has
        // (chromatic/root_note -- see sample_synth_tune_root_note_'s own
        // comment for the semitones<->root_note conversion), no new
        // engine API needed.
        Tune,
        // Batch-converts raw OP-1 .aif packs (from op1fun or the OP-1
        // pack builder) sitting in the SampleKit/SampleSound staging
        // folders into real, playable Kits/Sounds -- a wholly separate
        // feature from Capture's own Import source above (that one
        // pulls a single .wav from Grains' shared IMPORT/ folder into
        // the Capture buffer for manual Trim/Tune; this one goes
        // straight from a finished OP-1 patch to a finished Kit/Sound,
        // no Trim/Tune step, since a drum pack's 24 pre-sliced,
        // pre-mapped pads have no single buffer to trim, and a sampler
        // pack already carries its own precise pitch). See
        // TriggerSampleSynthPackImport()'s own doc comment for the
        // full mechanism. Positioned right before Preset since that's
        // "where the results show up."
        Import,
        // Load a Sound (Voice) or a Kit/Break (Drum) -- LOAD-ONLY, see
        // sample_synth_kit_cursor_'s own doc comment in this header.
        Preset,
        // Real Save/Load presets, one list per face -- SMPVO's own
        // references a Sound (see PerformanceStore::
        // SampleSynthVoiceUserPreset's own comment), SMPDR's own
        // references a Kit (SampleSynthDrumUserPreset) -- each bundled
        // with a full settings snapshot (Filter/Env/Mix/FX, Tune/
        // Mono-Poly, and for Drum, Setup's own Break Divide too).
        // Replaced the old bookmark-only Favorite (reference, no
        // settings) for exactly that reason. Same Save/Load shape
        // every other engine's own Preset page already has.
        User,
        kCount
    };

    // Preset page's "Files" branch own group chooser -- one level above
    // the factory-category/User folder list, entered right after
    // picking "Files" from the top-level Save/Load chooser. Added
    // because that flat folder list was already 54 factory categories +
    // User before any SysEx import existed (see
    // DexedSynth::kNumFactoryCategories's own doc comment on why so
    // many small categories exist in the first place -- one knob sweep
    // across a few hundred presets in ONE folder was already
    // unreliable); every completed import now adds one MORE folder
    // (PerformanceStore::DexedImportFolder), so a single flat sweep
    // across ever-growing dozens of folders was never going to scale.
    // Roms: the 4 real "Rom 1".."Rom 4" categories (real factory index
    //   0-3 -- literal unmodified Yamaha ROM cartridge contents).
    // Dexed: the other ~50 curated-by-sound-type categories (real
    //   factory index 4..kNumFactoryCategories-1).
    // Imports: one folder per completed SysEx import (see
    //   PerformanceStore::ListDexedImportFolders()), each holding just
    //   that one import's own contiguous slot range.
    // Hand-saved presets (every USER slot that ISN'T part of a recorded
    // import folder, filtered in RefreshDexedPresetSlots()) live on the
    // separate User page instead.
    enum class DexedFilesGroup
    {
        Roms,
        Dexed,
        Imports,
        kCount
    };

    // Screen::DexedOperator's own pages -- the v1 "core set" advanced
    // per-operator editor scope (Ratio/Level, Detune, and the 4 EG
    // Rates + 4 EG Levels via the existing AD/SR Button2-toggle idiom
    // already used for every ADSR-shaped page in this codebase -- see
    // dexed_op_egrate_target_sr_/dexed_op_eglevel_target_sr_). AmpModSens
    // was added in a later increment (see its own comment below);
    // VelocitySens and KeybRateScale (keyboard RATE scaling only -- the
    // full keyboard LEVEL scaling block, breakpoint + left/right depth +
    // curve, is a separate, bigger feature, still deliberately deferred)
    // were added after that, prompted by a real user report that a
    // low-velocity note sounded unexpectedly loud -- traced to this
    // patch's own velocity sensitivity byte being 0 (a real, legitimate
    // per-patch sound-design choice some patches make on purpose), with
    // no on-device way to see or change it until now. Operator select
    // (1-6, shown as the real HW op number, i.e. 6-dexed_op_index_) is
    // Button1-cycled, not a page of its own -- rotate stays reserved
    // for page navigation, matching every other screen's own
    // rotate-cycles-pages convention.
    enum class DexedOpParamPage
    {
        // Read-only reference view, same diagram Screen::Dexed's own
        // Algo page shows (see Ui::DrawAlgorithmDiagram(), shared
        // between both) -- added as the first page here after a real
        // user request, so you can see the current routing/carriers
        // while browsing per-operator pages without leaving this
        // screen. No knob of its own: Button1 is already committed to
        // operator-select duty on every page in this screen, so
        // there's no button left free to cycle the algorithm the way
        // Screen::Dexed's own Algo page does -- change it from there
        // instead.
        Algo,
        RatioLevel, // K1 = Coarse ratio (quantized 0-31), K2 = Output Level
        Detune,     // K1 only, 0-14 centered on 7
        EgRate,     // K1/K2 = Rate1/Rate2 (AD) or Rate3/Rate4 (SR), Button2 toggles
        EgLevel,    // K1/K2 = Level1/Level2 (AD) or Level3/Level4 (SR), Button2 toggles
        // K1 only, 0-3 (patch[off+14] & 3, ampmodsenstab in msfa) -- gates
        // whether this operator responds to the mod wheel's Amp/EG Bias
        // targets at all (0 = never, regardless of wheel position); see
        // DexedSynth::ModWheelTarget's own comment. Explicitly deferred in
        // the original advanced-editor plan, added once a real user
        // report ("I can hear no effect... on all of the cycles") traced
        // Amp/EG's silence to this byte never being exposed anywhere.
        AmpModSens,
        // K1 only, 0-7 (patch[off+15], read directly by msfa's own
        // ScaleVelocity() -- see dx7note.cpp). 0 means this operator's
        // output level never changes with velocity at all, by design --
        // some real DX7 patches (pads, organs, certain basses) are built
        // that way on purpose, not a bug.
        VelocitySens,
        // K1 only, 0-7 (patch[off+13], read directly by msfa's own
        // ScaleRate() -- see dx7note.cpp). How much playing higher up
        // the keyboard speeds up this operator's envelope rates; 0 means
        // every key uses the same rates regardless of pitch.
        KeybRateScale,
        kCount
    };

    // One entry per distinct (screen, page) knob assignment -- NOT per
    // layer, because cursor_layer_ can only change while screen_==Home,
    // and entering a Layer page always passes through Home first (which
    // is its own context), so a context change is always detected on the
    // way in regardless of which layer got selected. See ApplyKnobs().
    enum class KnobContext
    {
        Home,
        LayerStatus,
        LayerSpeed,
        LayerFilter,
        LayerEffect,
        LayerReverb,
        LayerGain,
        GlobalTempo,
        GlobalFilter,
        GlobalReverb,
        GlobalDelay,       // Global:Delay -- Time + Feedback
        GlobalDelayReverb, // Global:Dly-Rev -- Delay -> Reverb amount
        GlobalDelayTone,   // Global:DlyTone -- Ping-Pong + Tone
        // Global:Reverb Tone's own Button-toggled pair -- see
        // global_reverb_tone_target_b_.
        GlobalReverbTonePreDelay,     // PreDelay + PreFilter
        GlobalReverbToneDecayDamping, // DecayDiffusion + Damping
        // Global:Reverb Diffusion -- only 2 knobs (Input Diffusion 1+2),
        // no toggle needed.
        GlobalReverbDiffusion,
        GlobalBypassReverb, // Global:Bypass, B1 -- Reverb Send + shared Size
        GlobalBypassDelay,  // Global:Bypass, B2 -- Delay Send + shared Time
        GlobalSpeed,
        GlobalFile,
        GlobalExport,
        GlobalGranular, // entry point only -- see Screen::Granular instead
        GlobalLooper, // no continuous knobs -- Button1 toggle only
        GlobalMixer, // entry point only -- see Screen::Mixer instead
        GlobalSdMgmt, // browses a list directly, no pickup used -- see GlobalPage::SdMgmt
        GlobalDexed, // entry point only -- see Screen::Dexed instead
        GlobalEnsemble, // entry point only -- see Screen::Ensemble instead
        GlobalSampleSynth, // no continuous knobs -- Button1 toggle only, same as GlobalLooper
        // Screen::Granular's Grain page -- Button1/Button2 toggle which
        // pair the knobs reach (see granular_grain_target_gap_jitter_).
        GranularGrainSizeFill,
        GranularGrainGapJitter,
        GranularPosition,
        GranularScanRange,
        GranularScanGap,
        GranularScan,
        GranularTune,
        GranularEnvAD,
        GranularEnvSR,
        GranularFilter,
        GranularMix,
        GranularMixPanGain,
        // Screen::Granular's own new FX page -- Button1/Button2 toggle
        // between Reverb (own Send + shared Size) and Delay (own Send +
        // shared Time), same shape as GranularMix/GranularMixPanGain above
        // but a separate page so Mix's own Grain/Scan volumes don't need
        // to share room with 4 more FX parameters.
        GranularFxReverb,
        GranularFxDelay,
        GranularMidi, // K1 only -- which MIDI channel this engine listens on
        GranularCapture, // no continuous knobs -- Button1/Button2 only
        GranularTrim,
        GranularPreset, // browses a list directly, no pickup used
        GranularUser,   // ditto -- the User page's own list / save choice
        // Screen::Mixer -- shared across all 7 non-Master channels (see
        // mixer_position_), same "one context, branch internally on which
        // channel" idiom as LayerStatus already uses for Cur()/
        // cursor_layer_ -- except unlike LayerStatus, mixer_position_ CAN
        // change while this exact context stays active (rotating between
        // channels doesn't change context), so HandleEncoder()'s own
        // rotate handling must explicitly re-run the pickup reset/reseed
        // this enum's own change would normally trigger automatically
        // (see its own comment there).
        MixerVolPan,
        MixerReverb,
        MixerMaster, // Master has no toggle -- Volume + Reverb Size, always
        MixerNoKnobs, // Overview/Scope stops -- no continuous knobs
        // Screen::Dexed's own pages -- see DexedParamPage for what each
        // one's knobs do.
        DexedAlgo, // no continuous knobs -- Button1 cycles it
        DexedFeedback,
        DexedVibrato,
        // MOD page's own Button2 toggle -- K1 = patch[143]&7, the global
        // Pitch Mod Sensitivity byte that gates BOTH the automatic
        // Vibrato Depth knob above AND the wheel's Pitch target (see
        // DexedSynth::ModWheelTarget's own comment).
        DexedVibratoSens,
        DexedBrightness,
        DexedEnvSpeed,
        DexedFilter,
        DexedMix,
        DexedMixGain,
        // Screen::Dexed's own new FX page -- same shape/reasoning as
        // GranularFxReverb/GranularFxDelay above.
        DexedFxReverb,
        DexedFxDelay,
        DexedFold, // Wavefolder's own Amount + Trim pair -- no sub-mode toggle
        DexedMidi, // K1 only -- which MIDI channel this engine listens on
        DexedAdvanced, // entry point only -- see Screen::DexedOperator instead
        DexedPreset, // browses a list directly, no pickup used
        DexedUser,   // ditto -- the User page's own list / save choice
        DexedImport, // ditto -- the Import page's file list
        // Screen::DexedOperator -- one context per sub-page, shared
        // across all 6 operators (branch internally on dexed_op_index_),
        // same "one context, branch on which target" idiom LayerStatus
        // already uses for cursor_layer_/Cur(). EgRate/EgLevel each
        // split into two contexts for the AD/SR toggle, same shape as
        // GranularEnvAD/GranularEnvSR above.
        DexedOpAlgo, // view-only -- see DexedOpParamPage::Algo's own comment
        DexedOpRatioLevel,
        DexedOpDetune,
        DexedOpEgRateAD,
        DexedOpEgRateSR,
        DexedOpEgLevelAD,
        DexedOpEgLevelSR,
        DexedOpAmpModSens,
        DexedOpVelocitySens,
        DexedOpKeybRateScale,
        // Screen::Ensemble's own pages -- see EnsembleParamPage for what
        // each one's knobs do.
        EnsemblePreset, // browses a list directly, no pickup used
        EnsembleUser,   // ditto -- the User page's own list / save choice
        EnsembleRegistration,
        // Env page's own Button1/Button2 toggle, same shape as
        // GranularEnvAD/GranularEnvSR above.
        EnsembleEnvAD,
        EnsembleEnvSR,
        EnsembleEnsemble, // chorus depth + rate, no toggle needed
        EnsembleFilter,
        EnsembleMix,
        EnsembleMixGain, // Mix page, B2 -- K1 Gain into the limiter
        // Same shape as DexedFxReverb/DexedFxDelay above.
        EnsembleFxReverb,
        EnsembleFxDelay,
        EnsembleMidi, // K1 only -- which MIDI channel this engine listens on
        // Screen::SampleSynth's own pages -- same shapes as Dexed/
        // Grains' own Filter/Mix/FX, Env split AD/SR same as
        // GranularEnvAD/GranularEnvSR above.
        SampleSynthFilter,
        SampleSynthEnvAD,
        SampleSynthEnvSR,
        SampleSynthMix,
        SampleSynthMixGain,
        SampleSynthSetup, // K1 = Tune, K2 = Slices (pending) -- see that page's own comment
        SampleSynthFxReverb,
        SampleSynthFxDelay,
        SampleSynthMidi, // K1 only -- which MIDI channel this engine listens on
        SampleSynthCapture, // no continuous knobs outside Import mode -- see that page's own comment
        SampleSynthTrim,    // start/end, same shape as GranularTrim
        SampleSynthTune,    // root note only
        SampleSynthImport, // no continuous knobs -- Button1/Button2 only
        SampleSynthPreset, // browses a list directly, no pickup used
        SampleSynthUser,   // ditto -- browses whichever face's own User-preset list
        kCount
    };
    KnobContext CurrentKnobContext() const;
    // Soft pickup/takeover: a knob only starts driving its parameter once
    // its physical position is within kKnobPickupEpsilon of `stored_raw`
    // (the raw reading last applied for this (context, knob)), so
    // switching screens/pages doesn't yank a value to wherever the knob
    // physically happens to be sitting. ApplyKnobs() re-arms this
    // (engaged=false) every time the (screen, page) context changes --
    // that's not as heavy-handed as it sounds: the very first tick back
    // on a context re-checks proximity immediately (no actual wiggle
    // needed) and re-engages right away if the knob genuinely didn't
    // move while you were elsewhere. It only stays "caught" -- requiring
    // a real sweep -- when the knob DID move (e.g. you turned knob 1 to
    // set a layer's Volume, then went back to Home, where knob 1 also
    // controls Master Volume): that's exactly the case this exists to
    // guard against. DrawLayerScreen()/DrawHome() show each page's live
    // value specifically so it's visible what you're sweeping the knob
    // *to* -- same idea as a Pocket Operator's knob catch behavior.
    // Returns true if the caller should apply `raw` this tick.
    static bool KnobPickUp(float raw, float& stored_raw, bool& engaged);
    // What ApplyKnobs() actually calls: KnobPickUp() for the current
    // context, but only once that knob has been turned since arriving
    // here (see their own comment in ui.cpp).
    bool PickUpK1(float raw);
    bool PickUpK2(float raw);
    static constexpr float kPickupMove = 0.02f; // 2% of travel -- well above pot noise
    float k1_entry_raw_ = 0.f, k2_entry_raw_ = 0.f;
    bool  k1_moved_ = false, k2_moved_ = false;
    // Shared by Global:Speed's knob path (ApplyKnobs()) and its Button2
    // reset-to-1.0x -- two call sites, unlike most other global knobs
    // which only ever get set from one place.
    void SetProjectSpeed01(float v);
    // Seeds k1_pickup_raw_/k2_pickup_raw_[ctx] with this context's actual
    // current value(s) (e.g. Cur().GetFilterCutoff01() for LayerFilter),
    // called right where ApplyKnobs() re-arms `engaged` on a context
    // change. Without this, `stored_raw` is left at whatever it was last
    // set to for a *different* layer sharing this same context (or its
    // default of 0 if never engaged this session) -- so the knob would
    // need sweeping to that stale/zero position to re-engage instead of
    // to the value actually shown on screen.
    void SyncPickupTargets(KnobContext ctx);

    void HandleEncoder(const UiControlEvents& events);
    void HandleButton1(const UiControlEvents& events);
    void HandleButton2(const UiControlEvents& events);
    void OnButton1Short();
    void OnButton1Long();
    void OnButton1Release();
    void OnButton2Short();
    void ApplyKnobs();
    void UpdateLeds();
    void Draw();
    void DrawHome();
    void DrawLayerScreen();
    void DrawGlobalScreen();
    void DrawFileScreen();
    void DrawSdMgmtScreen();
    static const char* SdMgmtFolderName(SdMgmtFolder f);
    // Short form for the folder-picker grid's own tiny boxes (TomThumb,
    // ~32px wide) -- SdMgmtFolderName()'s full names stay for the
    // "now browsing X:" header once drilled into one.
    static const char* SdMgmtFolderShortLabel(SdMgmtFolder f);
    void DrawExportScreen();
    void DrawSpeedScreen();
    void DrawGranularScreen();
    void DrawMixerScreen();
    void DrawDexedScreen();
    void DrawSampleSynthScreen();
    // Screen::Mixer's own per-channel accessors -- channel index 0..3 is
    // Layer 1..4, 4 is Grains, 5 is Dexed, 6 is Ensemble, 7 is
    // SampleSynth, 8 is Bypass, 9 is Master. Kept as small indexed
    // switches rather than a polymorphic interface since there are only
    // 10 cases and they already read/write each engine's own real
    // getters/setters directly -- no new state of its own, this is
    // purely another view onto values that already exist.
    static constexpr int kNumMixerChannels = 10;
    // Total encoder "stops" in Screen::Mixer -- the real channels above
    // plus the Scope stop (mixer_position_ == kNumMixerChannels). See
    // Screen::Mixer's own comment.
    static constexpr int kNumMixerPositions = kNumMixerChannels + 1;
    const char* MixerChannelName(int ch) const;
    float       MixerGetVolume01(int ch) const;
    float       MixerGetPan01(int ch) const; // Master (last channel) has none -- returns 0.5f
    float       MixerGetSend01(int ch) const; // Master (last channel) has none -- returns 0.f
    void        MixerSetVolume01(int ch, float v01);
    void        MixerSetPan01(int ch, float v01);   // no-op on Master
    void        MixerSetSend01(int ch, float v01);  // no-op on Master
    // True for every channel except Master -- Dexed's own channel (ch 5)
    // used to be excluded here too, back when Dexed had no real Pan
    // control of its own; it now does (see DexedSynth::SetPan01()), so
    // this is just Master's own exception now.
    bool        MixerChannelHasPan(int ch) const;
    // Live level (0..1ish, not clamped -- see DrawMixerVBar's own clamp)
    // for the Overview grid and the Detail page's own Peak bar -- ch 0-3
    // read each layer's own LooperLayer::GetLevel(), 4-7 read the
    // Grains/Dexed/Bypass/Master globals main.cpp maintains (see
    // audio_engine.h). Unlike MixerGetVolume01() above, this is signal
    // level, not a fader setting -- it moves on its own regardless of
    // what any knob is set to.
    float MixerGetLevel01(int ch) const;
    // True while that channel's clip-indicator latch is active -- see
    // LooperLayer::IsClipping()'s own comment for why this is a hold,
    // not an instant flag.
    bool  MixerIsClipping(int ch) const;
    // Small helper for the Detail page's vertical bars and the Overview
    // grid's own single-bar-per-channel version. clip_lit draws a
    // filled cap at the very top of the bar's outline regardless of the
    // current fill level -- the clip-indicator latch, drawn this way
    // (rather than a separate element) so it costs no extra screen
    // space on either page.
    void DrawMixerVBar(int x0, int y0, int w, int h, float v01, bool clip_lit = false);
    // Live peak level -> 0..1 bar height on a -48..0 dB scale.
    static float MeterBar01(float level);
    // All 8 channels' names across one row, each with a real vertical
    // Volume bar (no numeric readout) -- GlobalPage::Mixer's at-a-glance
    // summary (see DrawGlobalScreen()). `top_y` is the name row's
    // baseline.
    void DrawMixerOverviewGrid(int top_y);
    // Attack/decay/sustain/release graph, four fixed-equal-width zones
    // (so turning one knob never visibly shifts another stage that
    // didn't change) -- shared by GranularParamPage::EnvAD/EnvSR, given
    // the actual curved seconds and sustain01 rather than reading an
    // engine pointer directly, so this stays reusable if anything else
    // ever wants the same graph.
    void DrawAdsrShape(int   top,
                        int   bottom,
                        float attack_s,
                        float decay_s,
                        float sustain01,
                        float release_s);
    // Real DX7 4-segment operator envelope (R1/L1..R4/L4) -- genuinely
    // different shape from DrawAdsrShape() above, not a reuse of it:
    // each of the 4 equal-width zones ramps from wherever the previous
    // segment left off to its OWN target level (rates/levels[0..3], raw
    // 0-99 patch bytes), holding flat for whatever's left of that zone
    // once it gets there -- unlike a classic ADSR, a real DX7 envelope
    // can rise and fall repeatedly across its 4 stages, so the fixed
    // up/down/flat/down topology DrawAdsrShape() assumes doesn't apply.
    // Higher rate = faster = reaches its target sooner within the zone
    // (the opposite direction from DrawAdsrShape()'s own seconds
    // parameters, where smaller = faster). Shared by
    // DexedOpParamPage::EgRate/EgLevel -- both show the same real shape
    // regardless of which pair is currently the editable one.
    void DrawDx7EnvelopeShape(int top, int bottom, const uint8_t rates[4], const uint8_t levels[4]);
    // Real box-and-arrow diagram of algo0's exact operator routing
    // (0-indexed, i.e. patch byte 134 as-is) -- generically derived from
    // FmCore::get_operator_routing() every call, never hand-authored per
    // algorithm. Originally DexedParamPage::Algo's own inline body;
    // extracted once Screen::DexedOperator's own Algo page (see
    // DexedOpParamPage::Algo) needed the identical diagram -- draws the
    // "ALGO N/32" label and the diagram itself, but deliberately NOT the
    // footer control rows below it, since Button1/Button2's own meaning
    // genuinely differs between the two screens (cycle algorithm vs.
    // cycle operator).
    void DrawAlgorithmDiagram(int algo0);
    // Live auto-scaled waveform trace from a small ring buffer -- shared
    // by whichever page wants a live timbre reference (Grains' own
    // Filter page shows its actual output changing as you turn its
    // knobs). Takes the buffer/capacity explicitly rather than reading a
    // fixed member, same reusability reasoning as DrawAdsrShape above.
    // cols: how many 2px columns wide, from x=1 -- full width by default,
    // narrower where a meter sits on the right (see DrawMixPageScope()).
    void DrawOscilloscope(int top, int bottom, const float* buf, size_t capacity,
                          int cols = LooperLayer::kWaveformCols);
    // A Mix page's body: scope on the left, the engine's level meter
    // (Global:Mixer channel ch's Peak bar, clip cap included) on the
    // right edge.
    void DrawMixPageScope(int top, const float* buf, size_t capacity, int mixer_ch);
    // Downsampled peak-bar waveform + optional moving playhead, shared by
    // Layer:Status (one layer's own GetWaveformPeaks()/GetPlayPos01()) and
    // Global:Speed (a composite max across all 4 layers) so the drawing
    // code exists exactly once.
    void DrawWaveform(const float* peaks, bool draw_playhead, float playhead_pos01);
    // Full-sample waveform for Screen::Granular's Grain page -- same
    // downsampled peak-bar technique as DrawWaveform() above, but with
    // TWO live markers instead of one (the Grain layer's fixed anchor
    // and the Scan layer's sweeping one) and its own vertical geometry,
    // since this page also needs room above the waveform for the live
    // Size/Fill/Gap/Scan readout that DrawWaveform()'s other callers
    // don't have. A separate function rather than extending DrawWaveform()
    // itself so Layer:Status/Global:Speed's existing, working display
    // can't regress.
    // grain_size01: fraction of the buffer one grain spans (see
    // GranularEngine::GetGrainSizeFraction01()) -- draws the Grain
    // marker as a span this wide, centered on grain_anchor01, instead of
    // a single-pixel tick, so Size is actually visible on the waveform.
    // 0 collapses back to a plain point marker (used by Trim's own reuse
    // of this same function for start/end points, where a size span
    // makes no sense).
    // range_start01/range_end01: when both are >= 0, draws a full-height
    // vertical line at each across the whole waveform band -- used by
    // the Scan Range page to show its own Start/End boundaries directly
    // on the display, separate from (and in addition to) the Grain/Scan
    // anchor ticks above. Negative (either one) disables this entirely,
    // the default for every other caller.
    void DrawGranularWaveform(const float* peaks,
                                float        grain_anchor01,
                                float        scan_anchor01,
                                bool         has_source,
                                float        grain_size01 = 0.f,
                                float        range_start01 = -1.f,
                                float        range_end01   = -1.f);
    // Encoder rotation while Global:Speed's transport mode is Scrub (see
    // speed_transport_mode_) -- nudges every non-empty layer's play_pos_
    // by the same raw-sample amount, keeping them all pointing at the
    // same shared timeline position, like scratching a physical tape loop.
    void ScrubBy(int32_t inc);
    // Encoder rotation while Global:Speed's transport mode is Freeze --
    // same per-layer raw-sample nudge as ScrubBy(), but moves each
    // layer's frozen drone window instead (LooperLayer::NudgeFreeze()),
    // so scrubbing selects which part of the loop the drone plays.
    void NudgeFreezeBy(int32_t inc);

    // --- SD save/load (Global:File page) -----------------------------
    // Refreshes file_slots_/file_slot_count_ from the card -- called
    // whenever the File page is entered and after a Save (a fresh slot
    // may now exist). Cheap (kMaxSlots f_stat() calls, only on a real SD
    // card access, not every frame).
    void RefreshFileSlots();
    // Button1 short: save the current in-memory performance. Overwrites
    // loaded_slot_ if a performance is currently loaded/was just saved;
    // otherwise saves to the next free slot and adopts it as
    // loaded_slot_ (see loaded_slot_'s comment). Button2 short
    // (force_new=true): always saves to a brand new slot regardless of
    // loaded_slot_, without touching whatever's currently loaded -- same
    // "never overwrites/destroys anything, plain tap is safe" class as
    // TriggerExport(), so this needs no hold-to-confirm either.
    void TriggerSave(bool force_new = false);
    // Global:File's own Load chooser confirmed with "New" highlighted
    // (see TriggerLoad() and load_new_selected_'s own comment) -- clears
    // every layer's audio (keeping tempo/global/per-layer settings, same
    // as the per-layer Clear()) and forgets loaded_slot_, so the next
    // Save lands in a new slot rather than overwriting whatever was
    // loaded -- the prior save on the card is untouched either way.
    void TriggerNew();
    // Load-list tap: loads one saved performance (PERFnnn), replacing
    // every layer's audio and all settings with the saved performance's.
    void TriggerLoad(int slot);

    // --- SD MGMT (Global:SdMgmt page) ------------------------------------
    // Returns whichever of file_slots_/granular_preset_user_slots_/
    // dexed_preset_user_slots_/ensemble_preset_user_slots_/
    // sample_synth_voice_user_numbers_/sample_synth_drum_user_numbers_
    // applies to sd_mgmt_folder_ (refreshing it first if that folder's
    // own dirty flag is set) and writes its count to *out_count.
    // nullptr/0 for the two bank-based boxes (Captures, Kits) -- see
    // SdMgmtIsBankFolder().
    const int* SdMgmtSlots(int* out_count);
    // Captures and Kits hold named items in numbered bank folders
    // (CAPTUREDn, KITSn/BREAKSn), not numbered slots, so inside those
    // two boxes SdMgmt first picks a bank (sd_mgmt_bank_ < 0), then an
    // item within it -- same two-level shape as the Sampler's own Preset
    // browser. These keep that view's own lists current.
    bool SdMgmtIsBankFolder() const
    {
        return sd_mgmt_folder_ == SdMgmtFolder::SampleSynthCaptures
               || sd_mgmt_folder_ == SdMgmtFolder::SampleSynthImports;
    }
    // CAPT box only: browsing Grains captures (GRAINS/CAPTUREDn) rather
    // than the Sampler's (SAMPLES/CAPTUREDn).
    bool SdMgmtGrainsCaptures() const
    {
        return sd_mgmt_folder_ == SdMgmtFolder::SampleSynthCaptures && sd_mgmt_capt_kind_ == 1;
    }
    // On a folder-list level inside a box (CAPT's Sampler/Grains choice,
    // or a bank list) -- K1 scrolls, B2 opens. Otherwise, once in a box,
    // it's the item list: K2 scrolls, B1 hold duplicates, B2 hold deletes.
    bool SdMgmtAtFolderList() const
    {
        return sd_mgmt_in_folder_
               && ((SdMgmtIsBankFolder() && sd_mgmt_bank_ < 0)
                   || (SdMgmtDrumUserBox() && sd_mgmt_drum_kind_ < 0));
    }
    // SMPDR box: user saves split into Kits / Breaks folders first, same
    // as the SMPDR User page (see sample_synth_drum_user_is_break_).
    // SMPIMP box, on one of its two choice levels (Voice/Drum, then
    // Sounds/Multi or Kits/Breaks).
    bool SdMgmtSmpChoosing() const
    {
        return sd_mgmt_folder_ == SdMgmtFolder::SampleSynthImports
               && (sd_mgmt_smp_kind_ < 0 || sd_mgmt_smp_sub_kind_ < 0);
    }
    bool SdMgmtDrumUserBox() const
    {
        return sd_mgmt_folder_ == SdMgmtFolder::SampleSynthDrumUserPresets;
    }
    // Entries at the current level (folder list or items) for K1/K2, and
    // the item cursor as an index into SdMgmtSlots()'s own array.
    int SdMgmtListCount();
    int SdMgmtItemIndex() const
    {
        return SdMgmtDrumUserBox() ? DrumUserGlobalIndex(sd_mgmt_drum_kind_, sd_mgmt_cursor_)
                                   : sd_mgmt_cursor_;
    }
    // Item count at whichever level SdMgmt is on inside a bank box.
    int  SdMgmtBankLevelCount();
    // Bank-box (Captures/Kits) halves of TriggerSdMgmtDuplicate()/Delete().
    void TriggerSdMgmtBankDuplicate();
    void TriggerSdMgmtBankDelete();
    void RefreshSdMgmtBanks();
    void RefreshSdMgmtItems();
    // B2 on the box grid: enter the highlighted box with a fresh scan.
    void SdMgmtOpenBox();
    // These 2 share their dirty flag/arrays with
    // SampleSynthParamPage::User's own inline refresh (ui.cpp's
    // DrawSampleSynthScreen()) -- same "whichever call runs first wins,
    // both consumers trust the same flag" convention. Replicated here
    // (not just the bare List* call) so the dirty flag's contract --
    // including each page's own cursor clamp -- stays identical no
    // matter which page actually triggers it.
    void RefreshSampleSynthVoiceUserSlots();
    void RefreshSampleSynthDrumUserSlots();
    // Button1 hold (800ms) while browsing a folder's files: duplicates
    // the browsed file via PerformanceStore::Duplicate*() -- same
    // "WORKING..." full-screen progress overlay as Save/Load for
    // whichever category can actually take real time (Performances,
    // Grains Presets).
    void TriggerSdMgmtDuplicate();
    // Button2 hold (see kSdMgmtDeleteHoldMs -- longer than the usual
    // 800ms, since unlike every other hold-to-confirm gesture in this
    // project, delete has no "just re-save/re-load" undo).
    void TriggerSdMgmtDelete();
    // Global:Tempo's Button2 held 800ms: saves the current global
    // settings (BPM/Bars/Volume/Metro/Filter/Reverb size/Bypass --
    // deliberately no per-layer settings) as the startup default -- see
    // ApplyStartupDefaults() and PerformanceStore::SavePrefs(). A hold,
    // not a tap, since this is a deliberate write, same weight as
    // Global:File's hold-to-Load/Layer:Status's hold-to-Clear.
    void TriggerSaveDefaults();
    // Layer:Status's Button2 short tap -- restores the cursor layer's
    // buffer from the shared Undo backup if (and only if) it's the one
    // that actually owns it right now (*undo_owner_layer_ ==
    // cursor_layer_); otherwise just sets undo_status_ to "Nothing to
    // undo" and leaves everything else alone. See
    // LooperLayer::RestoreFromBackup()/ConsumeOverdubSnapshotRequest()
    // for the snapshot/restore mechanics this depends on.
    void TriggerUndoOverdub();
    // Button1 short press on Global:Export: renders one full loop of the
    // current in-memory performance (every layer's real filter/effect/
    // reverb chain, plus the master filter) to a new WAV/EXPnnn.wav
    // file at the Pod's native 48kHz. Simple tap, no hold-to-confirm --
    // unlike Save, this never overwrites anything, it only ever creates a
    // new numbered file.
    void TriggerExport();
    // Button2 short press on Global:Export: same render as TriggerExport()
    // but resampled to 44100 Hz and written under custom/EXPnnn.wav
    // instead of WAV/EXPnnn.wav -- see PerformanceStore::ExportWav()'s
    // for_microdexed param. Plain tap, no hold-to-confirm, same reasoning
    // as TriggerExport() (never overwrites anything).
    void TriggerExportMicroDexed();
    static void OnSaveLoadProgress(float progress01); // PerformanceStore::ProgressFn
    // Rescales a sub-progress (e.g. "how far through THIS file's own
    // slots") into the overall bar's own 0..1 range, via the matching
    // g_progress_file_base01/_span01 globals TriggerSampleSynthPackImport()
    // sets before each file -- same "static state read by a plain
    // ProgressFn" shape OnSaveLoadProgress() itself already is, just
    // with one extra rescale step so a multi-file sweep's bar keeps
    // moving smoothly within each file, not just between them.
    static void OnSampleSynthImportFileProgress(float sub01); // PerformanceStore::ProgressFn

    // Instant copy of whichever loop layer granular_capture_source_
    // currently names (independent of Home's own cursor_layer_) into
    // Grains' own capture buffer, up to granular_capture_capacity_, then
    // points the engine at it via SetSource(). Runs from the main loop
    // (HandleButton2()), not the audio ISR -- a few hundred microseconds
    // to low milliseconds of copying is fine there, same reasoning as
    // SetSource()'s own waveform-peak rebuild.
    void TriggerGranularCaptureFromLayer();
    // Re-scans IMPORT/ on the SD card for .wav files -- called once on
    // entry to the Capture page's Import source (mirrors
    // RefreshGranularPresetSlots()'s own "dirty flag, refresh on entry"
    // idiom).
    void RefreshGranularImportFiles();
    // Loads/converts whichever file granular_import_cursor_ is currently
    // browsing into Grains' capture buffer (see
    // PerformanceStore::ImportWav()). Runs from the main loop with a live
    // progress overlay (Ui::OnSaveLoadProgress()), same as a Preset
    // load -- this involves real SD reads and per-sample conversion
    // (and possibly resampling), not an instant copy.
    void TriggerGranularImport();
    // Call whenever a new capture/load completes (Direct Record
    // finalizing, TriggerGranularCaptureFromLayer(),
    // TriggerLoadGranularPreset()) -- records the just-captured length as
    // the trim reference, rebuilds granular_trim_full_peaks_ over it, and
    // resets the trim points to 0..1 (no trim) so an old trim range never
    // silently carries over onto an unrelated new capture.
    void OnNewGranularCapture(size_t full_len);
    // Recomputes the sub-range passed to granular_->SetSource() from
    // granular_trim_start01_/end01_ against granular_capture_full_len_
    // (enforcing a minimum gap so the two points can't cross), and
    // applies it. The underlying buffer is untouched -- purely a matter
    // of which sub-range SetSource() currently points into -- so this can
    // be called repeatedly as the knobs move without losing anything.
    void ApplyGranularTrim();
    void RefreshGranularPresetSlots();
    // Button1 short tap (force_new=false): smart save -- overwrites
    // granular_loaded_preset_slot_ if it names a real slot, else a new
    // one. Button2 short tap (force_new=true): always a new slot. Both
    // save the CURRENTLY LOADED capture audio (via granular_->GetSourceL/
    // R()/GetSourceLen()) alongside the parameters -- same non-destructive
    // reasoning as TriggerSave()'s own force_new.
    void TriggerSaveGranularPreset(bool force_new = false);
    // Loads one numbered user save (User page), replacing both the live
    // parameters and the loaded capture audio.
    void LoadGranularUserSlot(int slot);
    // Loads the highlighted sound of the open Preset-page folder.
    void LoadGranularLibraryItem();
    // Trim page hold-B2: saves the current (trimmed) capture plus its
    // settings into the next free GRAINS/CAPTUREDn/CAPnnn.DAT, where it
    // then shows up on the Preset page.
    void TriggerSaveGranularCapture();
    // Folder index on the Preset page -> folder name under GRAINS/
    // ("PRESETS", "CAPTURED3") and on-screen label ("Starter", "Captured 3").
    void GranularLibFolder(int idx, char* name, size_t name_size, char* label,
                           size_t label_size) const;
    // "Bell Shimmer.DAT" -> "Bell Shimmer" for the screen.
    static void GranularLibDisplayName(const char* file, char* out, size_t out_size);
    // User page's "New (blank)" entry
    // -- clears the captured audio and resets every param to the
    // engine's own defaults, same fresh-start spirit as Global:File's
    // own TriggerNew().
    void TriggerNewGranularPreset();

    // --- Dexed presets (DexedParamPage::Preset) --------------------------
    // See DexedFilesGroup's own doc comment for the current 3-level
    // (group -> folder -> preset) shape -- was originally a flat 2-level
    // folder browser like the removed FmSynth's own, generalized further
    // once SysEx import made the flat folder list grow unbounded.
    // Re-scans BOTH the User slot list (filtered, see
    // dexed_preset_user_slots_'s own comment) and the import-folder
    // manifest (dexed_import_folders_) together -- the former's
    // filtering depends on the latter being current first.
    void RefreshDexedPresetSlots();
    int  ResolveDexedPresetSlot() const;
    // Real flat 0..(kNumFactoryCategories-1) factory category index for
    // the current dexed_files_group_ + dexed_preset_folder_cursor_
    // selection -- valid only when the group is Roms or Dexed (Imports/
    // User have no factory-category mapping at all, see
    // ResolveDexedPresetSlot()'s own per-group branches). Roms group
    // cursor 0-3 maps directly (already real); Dexed group cursor
    // 0..(kNumFactoryCategories-5) is offset by the 4 Roms categories
    // that come first in DexedSynth::kDexedFactoryCategories's own table.
    int ResolveDexedRealCategory() const;
    // Button1 short tap (force_new=false): smart save -- overwrites
    // dexed_loaded_preset_slot_ if it names a real (non-factory) slot,
    // else a new one. Button2 short tap (force_new=true): always a new
    // slot. Same non-destructive reasoning as TriggerSave()'s own
    // force_new.
    void TriggerSaveDexedPreset(bool force_new = false);
    // User page's "New" entry -- applies factory preset 0, same
    // fresh-start convention as the removed TriggerNewFmPreset().
    void TriggerNewDexedPreset();
    // Loads one preset by real slot number (factory or user), shared by
    // the Preset and User pages.
    void LoadDexedSlot(int slot);
    // Screen name for a slot: factory names straight from the firmware
    // table, a user slot's from its own file (cached per slot, see
    // dexed_browse_name_slot_), "DEXEDn" if it has no embedded name.
    const char* DexedSlotName(int slot);
    // Re-scans DXIMPORT/ on the SD card for .syx files -- called once on
    // entry to the Preset page's own Import file list, same pattern as
    // RefreshGranularImportFiles().
    void RefreshDexedImportFiles();
    // Hold-to-confirm-then-fire-once (see HandleButton2()'s own Dexed
    // Preset handling, same weight as TriggerGranularImport()'s own real
    // SD read). Reads/decodes whichever file dexed_import_cursor_ points
    // at (PerformanceStore::ImportDexedSyx()) and writes every voice it
    // contains into new, consecutively-numbered Dexed USER slots -- a
    // Single Voice Dump becomes one new slot, a 32-Voice Bulk Dump
    // becomes 32. The LAST voice imported is also applied live
    // (DexedSynth::ApplyPreset()) and becomes dexed_loaded_preset_slot_,
    // the same "land on something real" feel as TriggerLoadDexedPreset()'s
    // own preview.
    void TriggerDexedSyxImport();

    // --- Ensemble presets (EnsembleParamPage::Preset) --------------------
    // Re-scans the User slot list (ensemble_preset_user_slots_) --
    // flat, no import-folder filtering needed (Ensemble has no SysEx-
    // style bulk import).
    void RefreshEnsemblePresetSlots();
    // Button1 short tap (force_new=false): smart save -- overwrites
    // ensemble_loaded_preset_slot_ if it names a real user slot, else a
    // new one. Button2 short tap (force_new=true): always a new slot.
    // Same non-destructive reasoning as TriggerSave()'s own force_new.
    // Factory presets are never overwritable (see
    // ensemble_loaded_preset_slot_'s own 0-means-factory-or-none
    // comment), so this always writes to the User range.
    void TriggerSaveEnsemblePreset(bool force_new = false);
    // User page's "New (blank)" entry -- resets every parameter to
    // EnsembleSynth::EnsemblePresetData{}'s own defaults, same fresh-start
    // convention as TriggerNewDexedPreset().
    void TriggerNewEnsemblePreset();
    // Loads one preset by real slot number (1..factory count = factory,
    // above that = a user save) -- shared by the Preset and User pages.
    void LoadEnsembleSlot(int slot);
    // ensemble_loaded_preset_slot_ numbering: 0 = nothing/New, user
    // saves = their own file number (ENSM9..), factory preset i =
    // kEnsembleFactorySlotBase + i -- a separate range, so the factory
    // bank can grow without colliding with anyone's user saves.
    static constexpr int kEnsembleFactorySlotBase = 1000;
    static bool EnsembleIsFactorySlot(int s) { return s >= kEnsembleFactorySlotBase; }
    static bool EnsembleIsUserSlot(int s)
    {
        return s >= PerformanceStore::kEnsembleFirstUserSlot && s < kEnsembleFactorySlotBase;
    }
    // User page list: index 0 is "New (blank)", 1.. are the user saves.
    int ensemble_user_cursor_ = 0;

    // Two-row control legend, drawn at the bottom of every screen in
    // Tom Thumb (see font_tomthumb.h): a knob row (circle icon) and a
    // button row (square icon), each with a label flush to the screen
    // edge on either side and, for the knob row, a live value hugging the
    // icon. `left_value`/`right_value` may be "" to mean that side's
    // control does nothing on this page/state -- draw nothing rather than
    // a fake value (e.g. Speed/Gain/File pages only drive knob 1).
    // `square_icon` false draws a circle (knob row), true a square
    // (button row). `row_y` is the row's top pixel -- see kFooterRow1Y/
    // kFooterRow2Y below for the two fixed positions every screen uses.
    // `divider_y` draws a 1px horizontal rule at that y (pass -1 for no
    // divider). The two footer dividers use different gaps -- the
    // body/footer separator sits immediately above the knob row (0px
    // gap), while the knob/button row separator sits centered in a 3px
    // gap (1px blank, the line, 1px blank) -- so this takes the already-
    // resolved y-coordinate rather than deriving one from row_y, which
    // would get the second case wrong. See the k*Y constants below.
    // Every Font_6x8 label/value/status string on screen goes through
    // this instead of disp_->WriteString() directly, so it all renders
    // uppercase -- non-letters (digits, ':', '.', '%', ...) pass through
    // toupper() unchanged. Tom Thumb (the footer legend) is untouched --
    // it goes straight through DrawControlRow()/TomThumbDrawText(), never
    // this. Truncates past 63 chars (fine -- nothing drawn on this
    // 128px-wide display is remotely that long).
    void WriteUpper(const char* text);
    void DrawControlRow(int         row_y,
                         bool       square_icon,
                         int        divider_y,
                         const char* left_label,
                         const char* left_value,
                         const char* right_value,
                         const char* right_label,
                         bool       show_icon = true); // false: a readout, not a control
    // Master volume knob -> master_volume01_ and its cached curve.
    void SetMasterVolume01(float v01);
    // Fixed footer geometry, verified against the real Tom Thumb metrics
    // (row_h=6, baseline at the row's bottom pixel, icon vertically
    // centered on the rows text actually occupies -- see DrawControlRow()
    // for why these specific numbers). On a 128x64 display: divider line
    // y46, 1px gap, knob row y48-53, divider line y55, button row y57-62,
    // 1px empty margin y63. Both dividers sit with a 1px blank row on
    // either side of them, same convention -- kFooterDividerY is NOT
    // kFooterRow1Y-1 for that reason (confirmed on real hardware: that
    // adjacent placement reads as the line touching the row, no gap).
    static constexpr int kFooterDividerY      = 46; // body/footer separator (row1's divider)
    static constexpr int kFooterRow1Y         = 48; // knob row
    static constexpr int kFooterInterRowDividerY = 55; // row1/row2 separator (row2's divider)
    static constexpr int kFooterRow2Y         = 57; // button row
    // Live "where am I" indicator, drawn on every screen: one dot per
    // beat in the bar (current beat filled), plus a "B<n>" bar-in-loop
    // label when Bars > 1. Reads TempoClock::GetBeatInBar()/
    // GetBarInLoop() directly -- both tick regardless of whether the
    // metronome click is audible. `x`/`y` is the top-left of the "B<n>"
    // label's reserved space (a fixed width for up to "B16", so the
    // dots never shift as the bar-number digit count changes);
    // `dot_size` controls how large each beat square is (bigger/more
    // prominent on Home, compact when sharing a title row elsewhere).
    void DrawBeatIndicator(int x, int y, int dot_size);

    LooperLayer& Cur() { return layers_[cursor_layer_]; }

    daisy::DaisyPod*               pod_     = nullptr;
    daisy::OneBitGraphicsDisplay*  disp_    = nullptr;
    TempoClock*                    tempo_   = nullptr;
    LooperLayer*                   layers_  = nullptr;
    int                            num_layers_ = 0;

    Screen     screen_      = Screen::Home;
    int        cursor_layer_ = 0;
    LayerPage  layer_page_  = LayerPage::Status;
    GlobalPage global_page_ = GlobalPage::Tempo;

    bool encoder_long_fired_ = false;
    bool button1_long_fired_ = false;
    bool button2_long_fired_ = false;

    // Knob pickup state, one slot per KnobContext (see ApplyKnobs()).
    static constexpr size_t kNumKnobContexts = (size_t)KnobContext::kCount;
    KnobContext last_knob_context_                      = KnobContext::Home;
    float       k1_pickup_raw_[kNumKnobContexts]        = {};
    float       k2_pickup_raw_[kNumKnobContexts]        = {};
    bool        k1_pickup_engaged_[kNumKnobContexts]    = {};
    bool        k2_pickup_engaged_[kNumKnobContexts]    = {};

    // Last-committed raw pot reading, deadbanded in ApplyKnobs() against
    // AnalogControl's own filtered output -- that filter is a fast 2ms
    // slew meant to declick ADC steps, not reject the pot's own resting
    // noise/drift, so without this every knob-driven parameter slowly
    // wobbles even when the knob isn't touched.
    float k1_committed_ = 0.f;
    float k2_committed_ = 0.f;

    // Starting master_volume_ is computed properly through the same
    // curve ApplyKnobs() uses (see Ui::Init()), not hardcoded here, so
    // it stays consistent if that curve ever changes.
    float master_volume_   = 1.f;
    float master_volume01_ = 0.70f; // raw 0..1 last set by the knob, for save/restore
    // On by default -- overridden by a saved startup default if one
    // exists (see ApplyStartupDefaults()/PerformanceStore::LoadPrefs()),
    // same as every other field here.
    bool  bypass_          = true;

    FilterMode master_filter_mode_      = FilterMode::Off;
    float      master_filter_cutoff01_  = 0.5f;
    float      master_filter_res01_     = 0.f;

    float reverb_size01_ = 0.6f; // shared reverb bus's Size/decay, see GetReverbSize01()
    // The other 6 DattorroReverb parameters -- defaults match the
    // library's own tuned values (see DattorroReverb::Init()), with
    // Damping's default (0.05) converted through its own inverted
    // mapping so the knob's default still lands on the library's real
    // 0.95 internal value.
    float reverb_predelay01_          = 0.1f;
    float reverb_prefilter01_         = 0.85f;
    float reverb_input_diffusion1_01_ = 0.75f;
    float reverb_input_diffusion2_01_ = 0.625f;
    float reverb_decay_diffusion01_   = 0.70f;
    float reverb_damping01_           = 0.05f;
    // Global:Reverb Tone's own Button-toggled pair (PreDelay+PreFilter
    // vs. DecayDiffusion+Damping) -- same idiom as bypass_fx_target_delay_
    // below. Global:Reverb Diffusion has no toggle (only 2 knobs left).
    bool global_reverb_tone_target_b_ = false;
    float delay_time01_     = 0.f; // shared delay bus's Time, see GetDelayTime01() -- also
    // doubles as "delay bypassed" at this exact default (see main.cpp's
    // own delay_bypassed), so a fresh boot doesn't pay delay's fixed
    // per-sample cost until you actually turn the knob up.
    float delay_feedback01_ = 0.35f; // shared delay bus's Feedback, see GetDelayFeedback01()
    bool  delay_sync_        = false; // Global:Delay B1 -- Time follows the BPM
    float delay_pingpong01_  = 0.f;   // Global:DlyTone K1, see SharedDelay::Bus
    float delay_tone01_      = 0.f;   // Global:DlyTone K2
    float delay_to_reverb01_ = 0.f;   // Global:Dly-Rev K1
    // -1 = Omni (default) -- see GetDexedMidiChannel()/GetGranularMidiChannel()/
    // GetSampleSynthMidiChannel().
    int dexed_midi_channel_        = -1;
    int granular_midi_channel_     = -1;
    int ensemble_midi_channel_     = -1;
    int sample_synth_midi_channel_ = -1;
    // Global:Bypass's own Reverb/Delay target -- B1 = Reverb, B2 =
    // Delay, same as every engine's own FX page.
    bool bypass_fx_target_delay_ = false;
    // Same toggle, one per engine's own new FX page (Reverb Send+Size vs
    // Delay Send+Time) -- independent of Global:FX's own toggle above
    // and of each other.
    bool dexed_fx_target_delay_    = false;
    // MOD page's own Button2 toggle -- see KnobContext::DexedVibratoSens.
    bool dexed_vibrato_target_sens_ = false;
    bool granular_fx_target_delay_ = false;
    bool ensemble_fx_target_delay_ = false;
    // Ensemble Mix page: B1 = Level+Pan, B2 = Gain (see EnsembleSynth::SetGain01()).
    bool ensemble_mix_target_gain_ = false;
    // Same Level+Pan / Gain split on Dexed's and the Sampler's Mix pages.
    bool dexed_mix_target_gain_        = false;
    bool sample_synth_mix_target_gain_ = false;
    bool sample_synth_fx_target_delay_ = false;
    // Ensemble's own Env page toggle (Attack/Decay vs Sustain/Release) --
    // same idiom as GranularEnvAD/GranularEnvSR's own toggle.
    bool ensemble_env_target_sr_ = false;
    // SampleSynthParamPage::Env's own Button1/Button2 toggle (AD vs SR) --
    // same explicit-set convention as the FX toggles above.
    bool sample_synth_adsr_target_sr_  = false;
    // SampleSynthParamPage::Setup's own K1 (Tune) -- live, applied to
    // every active slot via SampleSynth::SetSlotPitchOffsetSemis() the
    // moment the knob moves (see that setter's own comment); tracked
    // here only so the page can display it and so a fresh Kit/Sound
    // load can reset it to 0 via OnSampleSynthContentLoaded() -- same
    // "settings don't leak from whatever was loaded before" reasoning
    // SampleSynth::mono_choke_'s own live engine state already follows.
    int8_t sample_synth_setup_tune_semis_ = 0;
    // Same page's own K2 (Slices) -- a *pending* value only, not yet
    // applied (Button2 held confirms it -- see
    // ApplySampleSynthBreakDivide()'s own doc comment). Reset by
    // OnSampleSynthContentLoaded() on every fresh load, to 16 if
    // nothing's configured yet, else whatever the currently-loaded
    // kit's own applied count already is.
    int sample_synth_setup_pending_slices_ = 16;
    // The count actually applied right now (0 = off) -- separate from
    // the pending value above so re-entering the page shows "what's
    // really active," not last session's unconfirmed knob position.
    int sample_synth_setup_applied_slices_ = 0;
    float bypass_reverb_send01_ = 0.f; // see GetBypassReverbSend01()
    float bypass_delay_send01_  = 0.f; // see GetBypassDelaySend01()
    float bypass_mix_volume01_  = 0.6f; // see GetBypassMixVolume01() -- 60% at boot, kept low against mic feedback
    float bypass_mix_volume_    = 1.f;  // curved, set properly in the ctor below
    float bypass_pan01_         = 0.5f; // see GetBypassPan01()

    // Global:Speed -- live-performance controls, deliberately NEVER
    // persisted (Save/Load/PREFS.DAT) -- same treatment as master volume.
    // 0.5f defaults to exactly 1.0x via SpeedCurve01()'s dead zone, so no
    // extra Init()-time recompute is needed the way master_volume_ needs.
    float project_speed01_   = 0.5f; // raw 0..1
    float project_speed_     = 1.f;  // actual multiplier, read every audio block
    SpeedTransportMode speed_transport_mode_ = SpeedTransportMode::Normal; // Global:Speed only -- see HandleEncoder()
    uint32_t last_scrub_tick_ms_ = 0; // for ScrubBy()'s turn-speed acceleration


    // Hardware PWM channels for LED1-Green/LED1-Blue/LED2-Red -- set once
    // by main.cpp's SetLedPwmChannels() after led_pwm_tim3 is initialized.
    // Null until then (UpdateLeds() guards every use), which only matters
    // for the brief window before main()'s own setup reaches that call.
    daisy::PWMHandle::Channel* led1_green_pwm_ = nullptr;
    daisy::PWMHandle::Channel* led1_blue_pwm_  = nullptr;
    daisy::PWMHandle::Channel* led2_red_pwm_   = nullptr;
    // Count-in flash on LED1 (see UpdateLeds()): the beat last seen and
    // when it started, so each beat lights for its first half.
    int      led_count_in_beat_    = -1;
    uint32_t led_count_in_beat_ms_ = 0;

    // --- Per-engine on/off (see IsGranularEnabled()).
    bool granular_enabled_ = false;
    bool looper_enabled_   = true; // see IsLooperEnabled()'s own comment for why true
    // Same false default as Granular's own now that Global:Dexed's real
    // on/off toggle exists (was temporarily true during Phase 4-6, back
    // when there was no UI toggle yet and defaulting off would have made
    // Dexed's own real MIDI dispatch unreachable/unheard).
    bool dexed_enabled_    = false;
    // Same false default as Dexed's own -- see IsEnsembleEnabled().
    bool ensemble_enabled_ = false;
    // Same false default as Dexed/Granular/Ensemble's own. Split into
    // two mutually-exclusive flags -- enabling one (Global:SMPVO/
    // Global:SMPDR's own Button1 toggle) forces the other false, so
    // the shared engine/arena/capture buffer is never claimed by both
    // "faces" at once. See IsSampleSynthEnabled() (true if either is
    // on) and SampleSynthMode's own comment.
    bool sample_synth_voice_enabled_ = false;
    bool sample_synth_drum_enabled_  = false;
    // Which "face" of the shared SampleSynth engine Screen::SampleSynth
    // currently shows -- NOT the same thing as which one is enabled
    // (you can browse either screen's pages while both are disabled);
    // this just decides which page set/title prefix is active and
    // which Preset/Import only ever lists (Sounds for Voice, Kits for
    // Drum). Set on entry via Global:SMPVO/Global:SMPDR's own encoder-
    // click (same entry-point idiom as Granular/Dexed/Ensemble), which
    // also triggers SwitchSampleSynthMode()'s own auto-restore.
    enum class SampleSynthMode
    {
        Voice,
        Drum
    };
    SampleSynthMode sample_synth_mode_ = SampleSynthMode::Voice;
    // Whether a given SampleSynthParamPage belongs to the current (or
    // a given) face -- Setup is Drum-only, Capture/Trim/Tune are
    // Voice-only, everything else (Filter/Env/Mix/FX/MIDI/Import/
    // Preset/User) is shared. Used by the page-rotate skip-loop in
    // HandleEncoder() so rotating never lands you on a page that
    // doesn't belong to the face you're actually on.
    static bool IsSampleSynthPageValidForMode(SampleSynthParamPage page, SampleSynthMode mode);
    // Called whenever Global:SMPVO/Global:SMPDR's own entry click
    // actually changes which face is showing (not on a same-face re-
    // click). Snapshots+restores Filter/Env/Mix/FX/Tune/Mono-Poly for
    // whichever face is being left/entered (see sample_synth_cap_bank_/
    // _imp_bank_'s own comment for Voice's own Cap/Imp split), and
    // re-loads whatever Kit/Sound belongs to the face being entered --
    // from SD for a Kit/Imported Sound (brief delay, since both share
    // the one Kit-loading arena), instantly from RAM for a still-
    // resident captured WAV (its own separate buffer, never touched
    // by Drum mode at all).
    void SwitchSampleSynthMode(SampleSynthMode target);

    uint32_t draw_counter_ = 0; // throttles the (slow, blocking-I2C) OLED redraw

    // See SaveLoadMode's own comment -- shared by Global:File and Grains
    // Preset. Reset to Idle whenever the page/screen
    // changes (see HandleEncoder()'s rotate handling), so leaving one of
    // these three pages never leaves it stuck mid-flow for the next visit.
    SaveLoadMode save_load_mode_ = SaveLoadMode::Idle;
    bool         save_as_new_    = false; // ChoosingSave's own Knob1 pick

    // BrowsingLoad's own two-level structure, mirroring ChoosingSave's
    // Overwrite/Save New chooser: entering Load first shows a top-level
    // "Files" vs "New" pick (Knob1), same as ChoosingSave's own pick --
    // load_new_selected_ is that pick, forced true when there's nothing
    // to browse (same "can't select what doesn't exist" reasoning as
    // Overwrite's own force-Save-New-when-nothing-loaded). Button1
    // tapped while "Files" is picked drills in (load_browsing_files_ =
    // true), handing Knob1 to the actual numbered list instead; Button1
    // tapped anywhere else in Load backs out one level (to the chooser,
    // or to Idle from the chooser itself) -- see OnButton1Short(). Reset
    // alongside save_load_mode_ (both wherever it's set to Idle and
    // wherever it's set to BrowsingLoad fresh).
    bool load_new_selected_   = false;
    bool load_browsing_files_ = false;

    // --- SD save/load (Global:File page) -----------------------------
    static constexpr int kMaxFileSlots = 99; // must match PerformanceStore::kMaxSlots
    int  file_slots_[kMaxFileSlots] = {};    // existing slot numbers, ascending
    int  file_slot_count_           = 0;
    int  file_cursor_               = 0;     // Load list: 0 = "New (blank)", 1.. = file_slots_
    // Which slot the in-memory performance currently corresponds to, or
    // -1 if it's a fresh/never-saved (or just-"New"'d) performance. Save
    // overwrites this slot when set; when -1, Save adopts the next free
    // slot and this becomes that slot number. See TriggerSave()/
    // TriggerNew()'s comments.
    int  loaded_slot_               = -1;
    bool file_slots_dirty_          = true; // forces one RefreshFileSlots() on first Draw()
    bool file_op_in_progress_       = false; // true only while inside Save()/Load()
    char file_status_[24]           = {};    // last result, shown briefly on the page

    // --- SD MGMT (Global:SdMgmt page) ------------------------------------
    // Reuses file_slots_/granular_preset_user_slots_ (and their own dirty
    // flags/Refresh*() calls) as the file list for whichever folder is
    // selected -- no separate copy of the same scan.
    // Delete gets a longer hold than every other confirm gesture in this
    // project (800ms elsewhere) -- unlike an overwrite or a wipe, there's
    // no "just re-save/re-load" undo for it.
    static constexpr float kSdMgmtDeleteHoldMs = 1500.f;
    SdMgmtFolder sd_mgmt_folder_    = SdMgmtFolder::Performances;
    bool         sd_mgmt_in_folder_ = false; // false = picking a folder, true = browsing its files
    int          sd_mgmt_cursor_    = 0;     // item list cursor (K2)
    int          sd_mgmt_list_cursor_ = 0;   // folder-list cursor (K1): Sampler/Grains, banks
    int          sd_mgmt_capt_kind_   = -1;  // CAPT box: -1 = choosing, 0 = Sampler, 1 = Grains
    int          sd_mgmt_drum_kind_   = -1;  // SMPDR box: -1 = choosing, 0 = Kits, 1 = Breaks
    int          sd_mgmt_smp_kind_      = -1; // SMPIMP box: -1 = choosing, 0 = Voice, 1 = Drum
    // SMPIMP's second choice: -1 = choosing; Voice -> 0 Sounds, 1 Multi;
    // Drum -> 0 Kits, 1 Breaks.
    int          sd_mgmt_smp_sub_kind_ = -1;
    char         sd_mgmt_status_[24] = {};   // last duplicate/delete result

    // --- WAV export (Global:Export page) ------------------------------
    bool export_op_in_progress_     = false; // true only while inside ExportWav()
    char export_status_[24]         = {};    // last result, shown briefly on the page

    // --- Startup defaults (Global:Tempo page) -------------------------
    char tempo_status_[24]          = {};    // last "save as default" result, shown briefly here

    // --- Undo last overdub (Layer:Status page) --------------------------
    // Unlike tempo_status_ (a settings page nobody idles on), this message
    // takes over the one thing Layer:Status is FOR -- the waveform/playhead
    // -- so, unlike every other transient status_ in this file, it can't
    // just persist until some unrelated action happens to overwrite it;
    // it needs its own timeout so an idle user gets their waveform back.
    // Only ever set to "Nothing to undo" (a successful Undo needs no
    // message of its own; the waveform/audio change is feedback enough).
    char     undo_status_[24]       = {};
    uint32_t undo_status_set_ms_    = 0;

    // --- Granular engine (Screen::Granular) -----------------------------
    GranularEngine*    granular_             = nullptr;
    GranularParamPage  granular_param_page_  = GranularParamPage::Grain;
    // false = knobs control Size+Fill, true = Gap+Jitter -- see
    // GranularParamPage::Grain's Button1/Button2 handling.
    bool granular_grain_target_gap_jitter_ = false;
    // false = knobs control Start+End, true = Gap (Scan's own, K1 only)
    // -- see GranularParamPage::ScanRange's Button1/Button2 handling,
    // same two-button pattern as the Grain page's own toggle.
    bool granular_scanrange_target_gap_ = false;
    // false = knobs control Attack+Decay, true = Sustain+Release -- see
    // GranularParamPage::ADSR's Button1/Button2 handling.
    bool granular_adsr_target_sr_ = false;
    // false = knobs control Grain+Scan volume, true = Pan+Gain -- see
    // GranularParamPage::Mix's Button1/Button2 handling.
    bool granular_mix_target_pan_gain_ = false;
    // Live output ring buffer for GranularParamPage::Filter's
    // oscilloscope -- same "engine writes samples, main.cpp owns the
    // buffer" convention as pad_scope_buf_ above.
    const float* granular_scope_buf_      = nullptr;
    size_t       granular_scope_capacity_ = 0;

    // --- Mixer (Screen::Mixer) ------------------------------------------
    // Live post-fader master mix ring buffer (captured in main.cpp right
    // after the master filter/click/master-volume stage -- the actual
    // final signal, not any one instrument's own output) for the
    // Mixer's own oscilloscope page. Same ownership convention as
    // pad_scope_buf_/granular_scope_buf_ above.
    const float* master_scope_buf_      = nullptr;
    size_t       master_scope_capacity_ = 0;
    // 0..3 = Layer 1..4, 4 = Grains, 5 = Dexed, 6 = Bypass, 7 = Master,
    // 8 = Overview, 9 = Scope -- see kNumMixerChannels/kNumMixerPositions/
    // MixerChannelName(). Changed by encoder rotate while on
    // Screen::Mixer (see HandleEncoder()'s own comment on why that needs
    // an explicit pickup reseed, unlike every other rotate-driven index
    // in this project).
    int  mixer_position_       = 0;
    // false = knobs control Volume+Pan, true = Reverb Send -- ignored on
    // Master and on the Overview/Scope stops (see MixerVolPan/
    // MixerReverb/MixerMaster/MixerNoKnobs's own KnobContext comment).
    bool mixer_target_reverb_  = false;

    // --- Grains capture (GranularParamPage::Capture) --------------------
    // Mutable (not const, unlike granular_scope_buf_ above) -- Ui writes
    // into these directly for the From-Layer copy, and main.cpp's
    // AudioCallback() writes into them sample-by-sample for Direct
    // Record. See main.cpp's g_granular_capture_l/r and
    // g_granular_capturing's own comments for the full split.
    float*           granular_capture_buf_l_      = nullptr;
    float*           granular_capture_buf_r_      = nullptr;
    size_t           granular_capture_capacity_   = 0;
    volatile bool*   granular_capturing_          = nullptr;
    volatile size_t* granular_capture_write_pos_  = nullptr;
    // -1 = Direct Record, -2 = Import (a WAV file from IMPORT/ on the SD
    // card), 0..num_layers_-1 = pull from that loop layer (independent of
    // cursor_layer_/Home's own selection) -- Button1 cycles through all
    // of these on the Capture page: Direct, Layer 1, Layer 2, Layer 3,
    // Layer 4, Import, back to Direct.
    int granular_capture_source_ = -1;
    // Edge-detection for Direct Record's press-to-start/release-to-stop
    // gesture (see HandleButton2()) -- also catches the ISR's own
    // auto-stop when the 5s buffer fills while Button2 is still held.
    bool granular_was_capturing_ = false;
    char granular_capture_status_[24] = {}; // last From-Layer/Import result

    // --- Grains WAV import (granular_capture_source_ == -2) -------------
    static constexpr int kMaxImportFiles = 16;
    // 61, matching PerformanceStore::kMaxImportWavNameLen+1 -- can't
    // reference that constant directly here since this header doesn't
    // include performance_store.h (only ui.cpp does, same reasoning as
    // GranularEngine being forward-declared instead of fully included).
    char granular_import_names_[kMaxImportFiles][61] = {};
    int  granular_import_file_count_ = 0;
    // Same "no factory range, browses a list directly" idiom as
    // granular_preset_cursor_ -- no continuous knob, just an index.
    int  granular_import_cursor_       = 0;
    bool granular_import_files_dirty_  = true; // forces one RefreshGranularImportFiles() on entry

    // --- Grains trim (GranularParamPage::Trim) --------------------------
    // The true, untrimmed length of whatever was most recently captured/
    // loaded (Direct Record, From Layer, or a Preset) -- GranularEngine's
    // own GetSourceLen() instead reflects whatever sub-range is CURRENTLY
    // active, so this is tracked separately as the reference the trim
    // points are computed against. Reset to 0..1 (no trim) every time a
    // new capture/load completes.
    size_t granular_capture_full_len_ = 0;
    float  granular_trim_start01_     = 0.f;
    float  granular_trim_end01_       = 1.f;
    // Throttles ApplyGranularTrim()'s own waveform-peaks recompute (an
    // O(full capture length) scan) to well below the ~1kHz main-loop/
    // knob-polling rate -- see GranularEngine::SetTrimRange()'s own doc
    // comment for why redoing that scan on every single tick was a real,
    // user-reported cause of Trim's knobs feeling laggy/slow to update.
    uint32_t granular_trim_peaks_throttle_ = 0;
    // Peaks over the FULL untrimmed capture (see GranularParamPage::Trim's
    // own comment) -- separate from GranularEngine::GetWaveformPeaks(),
    // which only ever reflects whatever sub-range is currently active.
    // 63, matching GranularEngine::kWaveformCols -- can't reference that
    // constant directly here since GranularEngine is only forward-
    // declared in this header (only ui.cpp includes the real definition).
    float granular_trim_full_peaks_[63] = {};

    // --- Grains presets (GranularParamPage::Preset) ---------------------
    static constexpr int kMaxGranularPresetSlots = 99; // must match PerformanceStore::kMaxGranularPresets
    int  granular_preset_user_slots_[kMaxGranularPresetSlots] = {}; // SD user slots, ascending
    int  granular_preset_user_slot_count_                      = 0;
    // User page list cursor: 0 = "New (blank)", 1.. = the user saves.
    int  granular_user_cursor_         = 0;
    bool granular_preset_slots_dirty_  = true; // forces one RefreshGranularPresetSlots() on entry
    // -1 = nothing loaded yet / current sound doesn't match a saved slot.
    int  granular_loaded_preset_slot_  = -1;
    char granular_preset_status_[24]   = {}; // last save/load result

    // --- Grains Preset-page library (GRAINS/PRESETS + CAPTUREDn) --------
    // Folder list: index 0 = Starter, 1.. = the Captured banks ascending.
    // Sizes match PerformanceStore::kMaxGranularCapturedBanks/
    // kGranularCapturedPerBank/kMaxGranularLibNameLen + 1 (not included here).
    uint8_t granular_captured_banks_[99]   = {};
    int     granular_captured_bank_count_  = 0;
    int     granular_lib_folder_           = -1; // open folder, -1 = at the folder list
    int     granular_lib_folder_cursor_    = 0;
    static constexpr int kGranularLibListMax = 32;
    char    granular_lib_names_[kGranularLibListMax][61] = {};
    int     granular_lib_count_            = 0;
    int     granular_lib_cursor_           = 0;
    bool    granular_lib_dirty_            = true; // folder list
    bool    granular_lib_items_dirty_      = true; // open folder's sounds
    // What's loaded from the library (empty = nothing), for "> " and "Now:".
    char    granular_loaded_lib_folder_[16] = {};
    char    granular_loaded_lib_name_[61]   = {};
    char    granular_trim_status_[32]       = {}; // last capture-save result

    // --- Dexed engine (Screen::Dexed) ------------------------------------
    DexedSynth*    dexed_            = nullptr;
    DexedParamPage dexed_param_page_ = DexedParamPage::Algo;
    // Live output ring buffer for Dexed's own oscilloscope pages
    // (Feedback/Brightness/EnvSpeed/Filter/Mix/FX) -- Dexed's own dry
    // output, same ownership convention as granular_scope_buf_ above.
    const float* dexed_scope_buf_      = nullptr;
    size_t       dexed_scope_capacity_ = 0;

    // --- Undo last overdub (Layer:Status, Button2 short tap) ------------
    // main.cpp-owned shared buffers/state -- see their own comments there
    // (declared alongside buffer_l/buffer_r) for the full picture. Ui
    // only reads undo_owner_layer_ (to check whether the cursor layer has
    // a pending Undo) and, on a successful Undo, writes -1 back through
    // it to invalidate ownership -- same "main.cpp owns the buffer, Ui
    // gets a pointer to use it" convention as every scope buffer above.
    float*  undo_backup_l_    = nullptr;
    float*  undo_backup_r_    = nullptr;
    int*    undo_owner_layer_ = nullptr;
    size_t* undo_backup_len_  = nullptr;

    // --- Dexed presets (DexedParamPage::Preset) --------------------------
    // Three levels now (see DexedFilesGroup's own doc comment for why):
    // group (Roms/Dexed/Imports/User) -> folder (skipped entirely for
    // User, which has no further sub-grouping) -> preset. dexed_files_group_
    // is which group; dexed_files_group_open_: false = K1 picks the
    // group, true = past it. Within a group, dexed_preset_folder_cursor_/
    // dexed_preset_folder_open_ mean "index of the highlighted folder
    // WITHIN this group" / "folder open, K1 now scrolls presets inside
    // it" for Roms/Dexed/Imports -- resolved to a real factory category
    // (see ResolveDexedRealCategory()) or a real PerformanceStore::
    // DexedImportFolder (see dexed_import_folders_) depending on the
    // group. User has no folder level at all -- dexed_files_group_open_
    // true for User means "browsing the flat, import-filtered slot list"
    // directly, same shape Global:File's own single-level list uses.
    DexedFilesGroup dexed_files_group_      = DexedFilesGroup::Roms;
    bool            dexed_files_group_open_ = false;
    int  dexed_preset_folder_cursor_ = 0;
    bool dexed_preset_folder_open_   = false;
    // Index WITHIN the open folder (not a flat index) -- resolved to a
    // real 1-based PerformanceStore slot by ResolveDexedPresetSlot().
    int  dexed_preset_cursor_ = 0;
    // Size of the User page's own list (hand-made saves only, imports
    // filtered out).
    static constexpr int kMaxDexedPresetSlots = 200;
    // Every slot above the 3129 factory presets, up to
    // PerformanceStore::kMaxDexedPresets (5500) -- what RefreshDexedPresetSlots()
    // scans before filtering imports out.
    static constexpr int kMaxDexedRawSlots = 2400;
    // Filtered to EXCLUDE any slot that belongs to a recorded import
    // folder (see RefreshDexedPresetSlots()) -- this is specifically the
    // User group's own slot list, not every user slot on the card.
    int  dexed_preset_user_slots_[kMaxDexedPresetSlots] = {}; // ascending
    int  dexed_preset_user_slot_count_                   = 0;
    bool dexed_preset_slots_dirty_ = true; // forces one RefreshDexedPresetSlots() on entry
    // One entry per completed SysEx import (see
    // PerformanceStore::SaveDexedImportFolder()), refreshed by the same
    // RefreshDexedPresetSlots() call/dirty flag above -- kept alongside
    // it (not a separate dirty flag) since the User list's own filtering
    // depends on this being current first.
    PerformanceStore::DexedImportFolder
         dexed_import_folders_[PerformanceStore::kMaxDexedImportFolders] = {};
    int  dexed_import_folder_count_ = 0;
    // Starts at slot 1 (factory preset 0, "ARP 2600"), matching
    // DexedSynth::Init()'s own boot default.
    int  dexed_loaded_preset_slot_ = 1;
    char dexed_preset_status_[24]  = {}; // last save/load result, shown briefly

    // Preset page's top-level chooser own third option -- Import (SD
    // .syx files, DXIMPORT/, separate from Grains' own WAV IMPORT/, see
    // PerformanceStore::ImportDexedSyx()). Mutually exclusive with
    // load_new_selected_ (Files is neither of these two) -- see the K1
    // 3-way quantization in ApplyKnobs()'s own DexedParamPage::Preset
    // case. dexed_import_browsing_: false = still at the top-level
    // chooser, true = drilled into the flat file list (one level, unlike
    // Files' own two -- there's no factory-category structure for an
    // arbitrary SD folder).
    // User page list cursor: 0 = "New", 1.. = dexed_preset_user_slots_.
    int  dexed_user_cursor_     = 0;
    int  dexed_import_cursor_   = 0; // which .syx file is highlighted
    char dexed_import_names_[kMaxImportFiles][PerformanceStore::kMaxImportSyxNameLen + 1] = {};
    int  dexed_import_file_count_  = 0;
    bool dexed_import_files_dirty_ = true; // forces one RefreshDexedImportFiles() on entry

    // Caches the real name (DexedSynth::GetPresetDataName()) of whichever
    // USER slot is currently highlighted while browsing "Load:" -- unlike
    // a factory slot's name (read straight out of a const embedded array,
    // free), a user slot's name lives inside its own saved file, so
    // showing it means an SD read; caching by slot number means that
    // only happens once per distinct slot the cursor lands on, not once
    // per Draw() tick while sitting still on one. -2 (never a real slot
    // number -- see ResolveDexedPresetSlot()'s own -1 "not found") means
    // nothing is cached yet.
    int  dexed_browse_name_slot_   = -2;
    char dexed_browse_name_buf_[11] = {};

    // --- Dexed advanced editor (Screen::DexedOperator) -------------------
    // 0-5, array index into patch_[] (op*21) -- 0 = HW OP6, 5 = OP1, same
    // convention confirmed from the real DX7 SysEx spec and already used
    // throughout the Algo diagram. Button1-cycled, not a page of its own.
    int                dexed_op_index_ = 0;
    DexedOpParamPage   dexed_op_page_  = DexedOpParamPage::Algo;
    // AD/SR toggles for the EgRate/EgLevel pages, same idiom as
    // granular_adsr_target_sr_ -- two independent bools since the two
    // pages' own toggle state shouldn't reset just from navigating
    // between them.
    bool dexed_op_egrate_target_sr_  = false;
    bool dexed_op_eglevel_target_sr_ = false;
    void DrawDexedOperatorScreen();
    void DrawEnsembleScreen();

    // --- Ensemble engine (Screen::Ensemble) ------------------------------
    EnsembleSynth*    ensemble_            = nullptr;
    EnsembleParamPage ensemble_param_page_ = EnsembleParamPage::Registration;
    // Live output ring buffer for Ensemble's own oscilloscope pages --
    // same ownership convention as dexed_scope_buf_ above.
    const float* ensemble_scope_buf_      = nullptr;
    size_t       ensemble_scope_capacity_ = 0;
    // Which of the 7 drawbars K2 currently edits on the Registration
    // page -- K1 browses this directly (same "no pickup, direct scrub"
    // idiom as Preset browsing elsewhere), K2 edits that one drawbar's
    // level with real pickup (see ApplyKnobs()'s own resync-on-change
    // comment, same idiom as dexed_op_index_/mixer_position_).
    int ensemble_drawbar_index_ = 0;

    // --- Ensemble presets (EnsembleParamPage::Preset) --------------------
    // Same flat "Files (numbered list)/Load New" shape as
    // GranularParamPage::Preset -- the small bundled factory bank (see
    // EnsembleSynth::GetNumFactoryPresets()) occupies the first
    // kEnsembleNumFactoryPresets entries of the SAME browsable list,
    // ensemble_preset_user_slots_ (real SD slot numbers, ascending) fills
    // the rest -- no separate category chooser needed, unlike Dexed's own
    // Roms/Dexed/Imports/User split (which only exists because Dexed has
    // 150+ factory patches, not 8).
    static constexpr int kMaxEnsemblePresetSlots = 99; // must match PerformanceStore::kMaxEnsemblePresets
    int  ensemble_preset_user_slots_[kMaxEnsemblePresetSlots] = {}; // SD user slots, ascending
    int  ensemble_preset_user_slot_count_                      = 0;
    // Preset page cursor: index into the factory sounds (slot = index + 1).
    int  ensemble_preset_cursor_      = 0;
    bool ensemble_preset_slots_dirty_ = true; // forces one RefreshEnsemblePresetSlots() on entry
    // 0 = nothing loaded yet / a factory preset is loaded (never
    // overwritable) -- same "<= 0 means force Save New" idiom as
    // granular_loaded_preset_slot_'s own 0 default, just 0 instead of -1
    // since a real user slot number is always >= 1 here too.
    int  ensemble_loaded_preset_slot_ = 0;
    char ensemble_preset_status_[24]  = {}; // last save/load result

    // --- SampleSynth engine (Screen::SampleSynth) ------------------------
    SampleSynth*         sample_synth_            = nullptr;
    SampleSynthParamPage sample_synth_param_page_ = SampleSynthParamPage::Filter;
    const float*         sample_synth_scope_buf_      = nullptr;
    size_t                sample_synth_scope_capacity_ = 0;
    const float*         bypass_scope_buf_            = nullptr;
    size_t                bypass_scope_capacity_       = 0;
    // Shared SDRAM arena -- see main.cpp's own g_sample_synth_arena_l/r
    // doc comment. Owned by main.cpp, just pointed at here, same
    // convention granular_capture_buf_l_/r_ already uses.
    float* sample_synth_arena_l_        = nullptr;
    float* sample_synth_arena_r_        = nullptr;
    size_t sample_synth_arena_capacity_ = 0;
    // Preset (kit) browsing state -- same shared SaveLoadMode/
    // load_browsing_files_ machinery Grains/Dexed's own Preset pages
    // use, but LOAD-ONLY: there's no on-device way yet to build a kit's
    // own slot assignments (that's a real, separate future feature --
    // see the plan's own "per-slot editor" discussion), so there's
    // nothing meaningful to Save back yet. Button2's short tap jumps
    // straight to BrowsingLoad (skipping Grains' own "Files vs New"
    // top-level chooser -- there's no "New" concept for a kit either).
    // Item cursor within the open bank -- shared by both faces' lists
    // (Sounds/Captured files in Voice, Kits/Breaks folders in Drum).
    int  sample_synth_kit_cursor_      = 0;
    // The resident kit's folder (PerformanceStore::kMaxSampleSynthKitRefLen),
    // empty while a single Sound or nothing is loaded.
    char sample_synth_loaded_kit_ref_[PerformanceStore::kMaxSampleSynthKitRefLen] = {};
    char sample_synth_loaded_kit_name_[PerformanceStore::kMaxSampleSynthKitNameLen + 1] = {};
    char sample_synth_kit_status_[24]  = {};
    void TriggerLoadSampleSynthKit();
    // Shared by TriggerLoadSampleSynthKit() (resolves the kit from the
    // browse cursor first), SwitchSampleSynthMode() and Drum User preset
    // loads (already have a remembered kit_ref) -- the real load logic.
    bool LoadSampleSynthKitByRef(const char* kit_ref);
    // The ORIGINAL per-pad manifest and combined arena span from the
    // most recent Kit load -- cached (not just a local in
    // TriggerLoadSampleSynthKit()) because Setup's own Button2 confirm
    // can run much later, and always recomputes fresh from these, not
    // from whatever the arena currently holds, so repeatedly changing
    // Slices never compounds/drifts. Empty (real_pad_count_==0) while a
    // single Sound is loaded instead of a Kit -- Setup's Slices/Button2
    // is a no-op in that case, nothing to re-divide.
    PerformanceStore::SampleSynthKitSlot
            sample_synth_kit_real_slots_[PerformanceStore::kMaxSampleSynthSlots];
    // Compacted (indices [0, real_pad_count_), load order, no gaps --
    // matches the arena's own back-to-back layout) rather than a raw
    // copy of the file's own possibly-sparse slot array.
    int32_t sample_synth_kit_real_pad_len_[PerformanceStore::kMaxSampleSynthSlots] = {};
    int     sample_synth_kit_real_pad_count_ = 0;
    size_t  sample_synth_kit_real_total_len_ = 0;
    // Setup page's own Break Divide -- see SampleSynthParamPage::Setup's
    // own doc comment. Called by Setup's own Button2-held confirm
    // (TriggerSampleSynthBreakDivideConfirm() below) to apply
    // sample_synth_setup_pending_slices_ instantly, no reload needed --
    // same "already in RAM, just recompute the pointers" reasoning
    // this whole feature is built on, see DESIGN.md. Always recomputes
    // from sample_synth_kit_real_slots_/_real_pad_count_/
    // _real_total_len_ above, never from the arena's current state, so
    // repeated changes never compound. divide_count==0 reverts to the
    // kit's real, originally-loaded per-pad mapping; tune_semis
    // applies to every active slot either way. A no-op while a single
    // Sound is loaded (real_pad_count_==0).
    void ApplySampleSynthBreakDivide(int divide_count, int8_t tune_semis);
    // Setup's own Button2-held confirm -- applies
    // sample_synth_setup_pending_slices_ (with the current
    // sample_synth_setup_tune_semis_) and updates _applied_slices_ to
    // match.
    void TriggerSampleSynthBreakDivideConfirm();
    // Resets sample_synth_setup_tune_semis_/_applied_slices_ to 0/0
    // (there's nothing to restore them FROM -- see
    // SampleSynthParamPage::Setup's own doc comment for why this is
    // pure live/session state, never saved with a kit), and seeds
    // _pending_slices_ to 16 -- see sample_synth_setup_pending_slices_'s
    // own comment. Called at the end of every Kit/Sound load, same
    // spirit as OnNewSampleSynthCapture().
    void OnSampleSynthContentLoaded(int applied_slices, int8_t tune_semis);

    // --- Play an individual sound directly, no kit needed -- a direct
    // request after kits alone felt too all-or-nothing: Button1 (Idle,
    // card present) toggles this page between browsing whole Kits
    // (above) and browsing raw files in SAMPLES/ one at a time (same
    // listing GranularEngine's own WAV import already proved out, see
    // ListSampleSynthFiles()). Loading a single sound clears every
    // slot and puts just that one file in slot 0, chromatic, full
    // keyboard range -- current ADSR/filter/output/pan/send settings
    // are left exactly as they are (unlike a kit load, which restores
    // its own saved settings), since this is "quickly audition a
    // sound," not "recall a whole configuration."
    // Items listed from one bank -- saves/imports fill a bank to 32
    // (PerformanceStore::kSampleBankCapacity); the headroom covers
    // files someone copied in by hand.
    static constexpr int kMaxSampleSynthSoundsListed = 64;
    // Both faces' banks together never exceed two types' worth.
    static constexpr int kMaxListedSampleBanks = 2 * PerformanceStore::kMaxSampleBanks;
    // "Unsaved changes" tracking -- each engine snapshots the settings a
    // load/save would capture, and the page title shows a small "*" while
    // the live sound differs from the snapshot taken at the last load or
    // save. Fields are appended explicitly (not memcpy'd structs) so
    // padding bytes can never make two equal sounds look different.
    struct EditSnapshot
    {
        uint8_t bytes[256];
        int     len = 0;
        template <typename T>
        void Add(const T& v)
        {
            if(len + (int)sizeof(T) <= (int)sizeof(bytes))
            {
                memcpy(bytes + len, &v, sizeof(T));
                len += (int)sizeof(T);
            }
        }
        bool operator==(const EditSnapshot& o) const
        {
            return len == o.len && memcmp(bytes, o.bytes, len) == 0;
        }
    };
    // Draws the title at the page's usual spot, plus a Tom Thumb "*" right
    // after it when `unsaved` -- shared by every engine's own Draw*Screen().
    void DrawPageTitle(const char* title, bool unsaved);
    // SMPVO/SMPDR: the live face's snapshot, its own baseline per face
    // (Voice/Drum keep separate sounds), and whether a fresh capture
    // hasn't been saved as a Sound yet.
    EditSnapshot SampleSynthSnapshot() const;
    void         MarkSampleSynthClean();
    void         SwitchSampleSynthModeImpl(SampleSynthMode target);
    bool         SampleSynthUnsaved() const;
    EditSnapshot sample_synth_voice_baseline_ = {};
    EditSnapshot sample_synth_drum_baseline_  = {};
    EditSnapshot EnsembleSnapshot() const;
    void         MarkEnsembleClean() { ensemble_baseline_ = EnsembleSnapshot(); }
    bool         EnsembleUnsaved() const
    {
        return ensemble_baseline_.len > 0 && !(EnsembleSnapshot() == ensemble_baseline_);
    }
    EditSnapshot ensemble_baseline_ = {};
    EditSnapshot dexed_baseline_    = {};
    EditSnapshot DexedSnapshot() const;
    void         MarkDexedClean() { dexed_baseline_ = DexedSnapshot(); }
    bool         DexedUnsaved() const
    {
        return dexed_baseline_.len > 0 && !(DexedSnapshot() == dexed_baseline_);
    }
    EditSnapshot GranularSnapshot() const;
    void         MarkGranularClean();
    bool         GranularUnsaved() const;
    EditSnapshot granular_baseline_      = {};
    bool         granular_capture_unsaved_ = false; // fresh capture not saved anywhere yet
    bool         sample_synth_capture_unsaved_ = false;
    struct SampleBankEntry
    {
        PerformanceStore::SampleBank type = PerformanceStore::SampleBank::Sounds;
        uint8_t                      number = 1;
        int8_t                       items  = -1; // -1 = not counted; else shown as "(N free)"
    };
    // "SOUNDS2" etc. -- the real folder under SAMPLES/.
    static void SampleBankEntryFolder(const SampleBankEntry& e, char* out, size_t out_size);
    // "Sounds 2", "Captured 1", "Kits 3", "Breaks 1" -- what the screen shows.
    static void SampleBankEntryLabel(const SampleBankEntry& e, char* out, size_t out_size);
    // Fills out[] with the face's banks of one type (see FaceBankType()),
    // ascending.
    static int ListFaceSampleBanks(bool drum, SampleBankEntry* out, int max_out, int type);
    // Marks every SampleSynth bank/item listing stale (Preset browser and
    // SdMgmt alike) -- after anything that adds/removes content.
    void MarkSampleSynthListsDirty();
    // Follows sample_synth_mode_ directly now (true for Voice, false
    // for Drum) -- no longer a user-facing toggle (SMPVO only ever has
    // Sounds to browse, SMPDR only ever has Kits), see
    // SwitchSampleSynthMode() for where this gets kept in sync. The
    // default here matches sample_synth_mode_'s own default (Voice).
    bool sample_synth_preset_mode_sounds_ = true;
    bool sample_synth_sounds_dirty_       = true; // re-scan the open bank's items on next draw
    // The open bank's items -- file names (Voice) or kit folder names
    // (Drum).
    char sample_synth_sound_names_[kMaxSampleSynthSoundsListed]
                                   [PerformanceStore::kMaxSampleSynthWavNameLen + 1] = {};
    int  sample_synth_sound_count_      = 0;
    // The current face's banks -- see ListFaceSampleBanks().
    // sample_synth_banks_drum_ records which face they were listed for.
    SampleBankEntry sample_synth_banks_[kMaxListedSampleBanks] = {};
    int             sample_synth_bank_count_  = 0;
    bool            sample_synth_banks_dirty_ = true;
    bool            sample_synth_banks_drum_  = false;

    // --- Global:SdMgmt's own bank-box view (declared here, after
    // SampleBankEntry, rather than with the rest of SdMgmt's state).
    // Bank-box state (Captures/Kits only, see SdMgmtIsBankFolder()):
    // which bank is open (-1 = picking one), the banks on offer, and the
    // open bank's items. Its own arrays, not the Preset browser's, so
    // neither view can leave the other pointing at a stale list.
    int             sd_mgmt_bank_ = -1;
    SampleBankEntry sd_mgmt_banks_[kMaxListedSampleBanks] = {};
    int             sd_mgmt_bank_count_  = 0;
    bool            sd_mgmt_banks_dirty_ = true;
    char            sd_mgmt_item_names_[kMaxSampleSynthSoundsListed]
                                [PerformanceStore::kMaxSampleSynthWavNameLen + 1] = {};
    int             sd_mgmt_item_count_  = 0;
    bool            sd_mgmt_items_dirty_ = true;
    char sample_synth_loaded_sound_name_[PerformanceStore::kMaxSampleSynthWavNameLen + 1] = {};
    // The real SD folder sample_synth_loaded_sound_name_ came from
    // (a SOUNDSn or CAPTUREDn bank) -- empty when nothing's
    // loaded (or a Kit is loaded instead of an individual Sound). Only
    // tracked for display/status only now -- sample_synth_voice_sound_
    // name_/_category_ (its own comment) is the one Save/mode-switch
    // actually read, since this one gets stomped by a Drum-side Kit load.
    char sample_synth_loaded_sound_category_[12] = {};
    void TriggerLoadSampleSynthSound();
    // Shared by TriggerLoadSampleSynthSound()/
    // TriggerLoadSampleSynthVoiceUserPreset()/SwitchSampleSynthMode()
    // -- see its own doc comment in ui.cpp for why.
    void LoadSampleSynthFileIntoEngine(const char* category_folder, const char* filename);

    // --- SMPVO's own real User presets -- see
    // PerformanceStore::SampleSynthVoiceUserPreset's own doc comment
    // for why this replaced the old bookmark-only Favorite (reference
    // plus a full settings snapshot now, not just a reference). Same
    // Save/Load shape every other engine's own Preset page already
    // has (save_load_mode_/save_as_new_ are the SAME shared fields
    // Dexed/Ensemble/Grains Preset already use -- see those own
    // comments): Button1 tap -> ChoosingSave (Knob1 picks Overwrite/
    // New, Button1 held commits), Button2 tap -> BrowsingLoad (Knob1
    // scrolls, Button2 held commits Load). K1 scrolls the list, same
    // convention as Preset's own Kits/Sounds lists.
    bool sample_synth_voice_user_dirty_ = true; // re-scan (load from card) on first entry
    int  sample_synth_voice_user_numbers_[PerformanceStore::kMaxSampleSynthVoiceUserPresets] = {};
    int  sample_synth_voice_user_count_       = 0;
    int  sample_synth_voice_user_cursor_      = 0;
    // 0 = nothing loaded from a User slot yet (Overwrite isn't offered
    // -- same "0 means none" convention dexed_loaded_preset_slot_'s
    // own factory/user split uses, just without a factory range here).
    int  sample_synth_voice_user_loaded_slot_ = 0;
    char sample_synth_voice_user_status_[24]  = {};
    void TriggerSaveSampleSynthVoiceUserPreset(bool force_new);
    void TriggerLoadSampleSynthVoiceUserPreset();

    // --- SMPDR's own counterpart, same shape -- see
    // PerformanceStore::SampleSynthDrumUserPreset's own doc comment.
    bool sample_synth_drum_user_dirty_ = true;
    int  sample_synth_drum_user_numbers_[PerformanceStore::kMaxSampleSynthDrumUserPresets] = {};
    int  sample_synth_drum_user_count_       = 0;
    int  sample_synth_drum_user_cursor_      = 0;
    int  sample_synth_drum_user_loaded_slot_ = 0;
    // SMPDR User page: Kits / Breaks folders in front of the saves (shared
    // browse convention). Each save is sorted by the kit folder it was
    // made from (its kit_ref starts "KITS" or "BREAKS"), read once per
    // RefreshSampleSynthDrumUserSlots().
    int  sample_synth_drum_user_folder_        = -1; // -1 = folder list, 0 = Kits, 1 = Breaks
    int  sample_synth_drum_user_folder_cursor_ = 0;
    bool sample_synth_drum_user_is_break_[PerformanceStore::kMaxSampleSynthDrumUserPresets] = {};
    int  sample_synth_drum_user_folder_count_[2] = {};
    // Screen names for the SMPVO/SMPDR user saves, same idea as Dexed's: the
    // sound file (SMPVO) or kit/break folder (SMPDR) each was made from,
    // without path or extension -- filled by the Refresh*UserSlots() above.
    static constexpr int kUserSaveNameLen = 20;
    char sample_synth_voice_user_names_[PerformanceStore::kMaxSampleSynthVoiceUserPresets]
                                       [kUserSaveNameLen] = {};
    char sample_synth_drum_user_names_[PerformanceStore::kMaxSampleSynthDrumUserPresets]
                                      [kUserSaveNameLen] = {};
    // "BREAKS1/Amenbreak" / "SKOGSFIOL C4.wav" -> "Amenbreak" / "SKOGSFIOL C4".
    static void SourceDisplayName(const char* src, char* out, size_t out_size);
    // The name to show for user save i of either face, falling back to
    // "SMPVOn"/"SMPDRn" if it has none.
    void SampleSynthUserSaveName(bool voice, int i, char* out, size_t out_size) const;
    // k-th save inside a folder -> its index in sample_synth_drum_user_numbers_
    // (-1 if none), and the reverse.
    int DrumUserGlobalIndex(int folder, int k) const;
    int DrumUserFolderPos(int global) const;
    char sample_synth_drum_user_status_[24]  = {};
    void TriggerSaveSampleSynthDrumUserPreset(bool force_new);
    void TriggerLoadSampleSynthDrumUserPreset();

    // Both faces browse in two levels: first a bank (index into
    // sample_synth_banks_ -- "Sounds 1", "Captured 2" in Voice; "Kits 1",
    // "Breaks 1" in Drum), then that bank's items. Captured stays its own
    // bank type rather than merged into Sounds, since Capture/Trim is a
    // distinct workflow from importing a pack and the user asked for them
    // to stay visibly separate; Breaks likewise. -1 = still at bank-select
    // (Knob1 scrolls sample_synth_sound_category_cursor_); >= 0 = drilled
    // into that bank's items (Knob1 scrolls sample_synth_kit_cursor_).
    // Button2's short tap drills in. Resets to -1 on every fresh entry,
    // same "start over at the top" reasoning sample_synth_kit_cursor_'s
    // own reset already follows.
    int sample_synth_sound_category_        = -1;
    int sample_synth_sound_category_cursor_ = 0;
    // Global:Mixer: Button1 swaps the overview for per-stage CPU
    // (profiler.h).
    bool mixer_profile_view_ = false;
    // One level above the banks on the Preset page: which type -- SMPVO
    // 0 Sounds, 1 Captured, 2 Multi; SMPDR 0 Kits, 1 Breaks (see
    // FaceBankType()); -1 = still choosing (Knob1 scrolls
    // sample_synth_voice_type_cursor_).
    int sample_synth_voice_type_        = -1;
    int sample_synth_voice_type_cursor_ = 0;
    int sample_synth_banks_voice_type_  = -1; // what sample_synth_banks_ was listed for
    static constexpr int kNumVoiceBankTypes = 3;
    static constexpr int kNumDrumBankTypes  = 2;
    int NumFaceBankTypes() const
    {
        return sample_synth_preset_mode_sounds_ ? kNumVoiceBankTypes : kNumDrumBankTypes;
    }
    static PerformanceStore::SampleBank FaceBankType(bool drum, int i)
    {
        using PerformanceStore::SampleBank;
        if(drum)
            return i == 1 ? SampleBank::Breaks : SampleBank::Kits;
        return i == 0 ? SampleBank::Sounds : i == 1 ? SampleBank::Captured : SampleBank::Multi;
    }
    // At the Preset page's type list rather than its banks.
    bool AtSampleSynthVoiceTypeSelect() const { return sample_synth_voice_type_ < 0; }

    // --- SampleSynth Capture/Trim/Tune (SampleSynthParamPage::Capture/
    // Trim/Tune) -- same Direct Record/From Layer/Import workflow as
    // GranularParamPage::Capture/Trim, but always into slot 0 and saved
    // out as a real Sound file (see TriggerSaveSampleSynthCapture())
    // rather than Grains' own self-contained preset. Mutable (not
    // const) -- Ui writes into these directly for the From-Layer copy,
    // and main.cpp's AudioCallback() writes into them sample-by-sample
    // for Direct Record, same split as granular_capture_buf_l_/r_.
    float*           sample_synth_capture_buf_l_     = nullptr;
    float*           sample_synth_capture_buf_r_     = nullptr;
    size_t           sample_synth_capture_capacity_  = 0;
    volatile bool*   sample_synth_capturing_         = nullptr;
    volatile size_t* sample_synth_capture_write_pos_ = nullptr;
    // -1 = Direct Record, -2 = Import (IMPORT/ on the SD card -- reusing
    // Grains' own folder rather than adding a third near-identical one,
    // since this is a one-time external-WAV staging step, not a
    // persisted/browsable content library the way SAMPLES/ itself is),
    // 0..num_layers_-1 = that loop layer. Same Button1 cycle idiom as
    // granular_capture_source_.
    int  sample_synth_capture_source_ = -1;
    bool sample_synth_was_capturing_  = false; // edge-detection, mirrors granular_was_capturing_
    char sample_synth_capture_status_[24] = {}; // last From-Layer/Import/Save result

    // --- SampleSynth WAV import (sample_synth_capture_source_ == -2) ----
    static constexpr int kMaxSampleSynthImportFiles = 16;
    char sample_synth_import_names_[kMaxSampleSynthImportFiles][61] = {};
    int  sample_synth_import_file_count_ = 0;
    int  sample_synth_import_cursor_     = 0;
    bool sample_synth_import_files_dirty_ = true; // forces one refresh on entry

    // --- SampleSynth trim (SampleSynthParamPage::Trim) ------------------
    // The true, untrimmed length of whatever was most recently captured
    // -- slot 0's own len instead reflects whatever sub-range is
    // CURRENTLY active, so this is tracked separately as the reference
    // the trim points are computed against, same split as Grains' own
    // granular_capture_full_len_.
    size_t sample_synth_capture_full_len_ = 0;
    float  sample_synth_trim_start01_     = 0.f;
    float  sample_synth_trim_end01_       = 1.f;
    // Peaks over the FULL untrimmed capture -- 63, matching
    // GranularEngine::kWaveformCols (reused here as the fixed column
    // count for DrawGranularWaveform(), a generic Ui:: method despite
    // its name -- see that page's own comment in ui.cpp). Computed once
    // per new capture (OnNewSampleSynthCapture()); unlike Grains' own
    // equivalent, SampleSynth has no second "live active sub-range"
    // waveform page that also needs a throttled per-tick recompute --
    // Trim here always shows the full capture with markers, nothing
    // else reads a sub-range view, so no throttle is needed.
    float sample_synth_capture_peaks_[63] = {};

    // --- SampleSynth tune (SampleSynthParamPage::Tune) ------------------
    // Direct UI for fields SampleSlot already has -- see SampleSynth::
    // LoadSlot()'s own params. Preserved across Trim adjustments (not
    // reset when start01/end01 change) and re-applied on every Tune
    // change via the same ApplySampleSynthTrim() call Trim itself uses.
    // root_note_ is the ONLY one of these two actually stored (there's
    // no separate "semitones" field) -- the knob maps to it via
    // `root_note = 60 - semitones` (see ApplyKnobs()'s own
    // SampleSynthParamPage::Tune case), so 60 here means 0 semitones,
    // i.e. "play back exactly as captured" -- same feel as Grains' own
    // Tune knob, which is centered on zero the same way. The old
    // "toggle chromatic on/off" is gone -- captures are always
    // chromatic now (direct request, once SMPVO/SMPDR split so this
    // engine's own melodic content never needs to be a fixed-pitch
    // one-shot).
    uint8_t sample_synth_tune_root_note_  = 60;
    // K2/B2 on the same Tune page -- Tune/Mono-Poly for whatever's
    // loaded via Import/Preset/User, independent of the K1/B1 pair
    // above (which are Capture-specific). Cap and an Imported Sound
    // are mutually exclusive (loading one still clears the other) but
    // each needs its own remembered setting that doesn't bleed into
    // the other -- see sample_synth_voice_source_'s own comment for
    // how these get applied/snapshotted on every switch.
    int8_t sample_synth_imp_tune_semis_ = 0;
    bool   sample_synth_cap_mono_       = false; // Poly by default, confirmed
    bool   sample_synth_imp_mono_       = false;
    // Which of Cap/Imp is the CURRENT source of SMPVO's slot 0+ --
    // None right after a wipe (ClearSampleSynthSlotsForFreshCapture()/
    // entering Voice mode with nothing loaded yet). Drives which bank
    // SwitchSampleSynthMode()/a fresh capture/an Import-Preset-User
    // load snapshots INTO before switching away, and which Tune/Mono-
    // Poly pair (cap_* vs imp_*) a live knob/button change actually
    // applies to right now.
    enum class SampleSynthVoiceSource
    {
        None,
        Captured,
        Imported
    };
    SampleSynthVoiceSource sample_synth_voice_source_ = SampleSynthVoiceSource::None;
    // Filter/Env/Mix/SR-FX snapshots for whichever of Cap/Imp is NOT
    // currently active -- see SwitchSampleSynthMode()'s own comment
    // for when these get captured/applied (a lazy snapshot-on-switch,
    // not a live mirror on every knob tick, so Filter/Env/Mix/FX's own
    // ApplyKnobs() cases need no changes at all).
    SampleSynth::SampleSynthPresetData sample_synth_cap_bank_;
    SampleSynth::SampleSynthPresetData sample_synth_imp_bank_;
    // Same idea, one level up: Drum's own Filter/Env/Mix/SR-FX,
    // snapshotted/restored across a Voice<->Drum mode switch the same
    // way, so turning on a low-pass Filter while on Drum doesn't bleed
    // into whatever Voice had set the next time you switch back.
    SampleSynth::SampleSynthPresetData sample_synth_drum_bank_;
    // What to reload when switching back into Voice mode with an
    // Imported Sound as the last-known source (SampleSynthVoiceSource::
    // Imported) -- separate from sample_synth_loaded_sound_name_/
    // _category_ (which just reflect whatever's CURRENTLY resident in
    // the shared arena right now, and get stomped by a Drum-mode Kit
    // load) so a Drum-side load can never clobber what Voice itself
    // should come back to.
    char sample_synth_voice_sound_name_[PerformanceStore::kMaxSampleSynthWavNameLen + 1]     = {};
    char sample_synth_voice_sound_category_[12] = {};
    // Same idea for Drum mode's own last-known Kit -- separate from
    // sample_synth_loaded_kit_ref_ for the same reason. Empty = none.
    char sample_synth_drum_kit_ref_[PerformanceStore::kMaxSampleSynthKitRefLen] = {};
    // Drum's own Setup state (Break Divide count + Tune), snapshotted
    // when LEAVING Drum mode and re-applied after the Kit itself
    // reloads when RETURNING -- otherwise LoadSampleSynthKitBySlot()'s
    // own OnSampleSynthContentLoaded(0, 0) call resets Setup back to
    // off/0 on every reload, silently dropping a split the moment you
    // switch away and back (a real reported bug: "it has the sound but
    // does not keep the split").
    int    sample_synth_drum_remembered_slices_     = 0;
    int8_t sample_synth_drum_remembered_tune_semis_ = 0;
    // Each face remembers its OWN last-visited page, restored on
    // switching back in -- Filter by default (power-on/first-ever
    // visit to that face), but NOT force-reset on every switch after
    // that (direct correction: "didn't want it to always reset to
    // Filter, only on startup"). Keeps Voice's own browsing position
    // independent of wherever Drum happens to be sitting -- without
    // these, FX (valid on both) would otherwise carry the raw shared
    // sample_synth_param_page_ straight across, landing Voice on
    // whatever page Drum was last left on.
    SampleSynthParamPage sample_synth_voice_last_page_ = SampleSynthParamPage::Filter;
    SampleSynthParamPage sample_synth_drum_last_page_  = SampleSynthParamPage::Filter;

    void TriggerSampleSynthCaptureFromLayer();
    void RefreshSampleSynthImportFiles();
    void TriggerSampleSynthImport();
    // Common finalize step for all 3 capture sources -- resets trim to
    // 0%/100% and rebuilds sample_synth_capture_peaks_ over the new
    // full_len, mirrors Ui::OnNewGranularCapture().
    void OnNewSampleSynthCapture(size_t full_len);
    // Shared by ApplySampleSynthTrim() and TriggerSaveSampleSynthCapture()
    // -- both need the same start01_/end01_ -> real sample-index range,
    // with the same 1% minimum-gap guard Grains' own ApplyGranularTrim()
    // uses, and must never drift out of sync with each other.
    void ComputeSampleSynthTrimRange(size_t* out_start, size_t* out_end) const;
    // Recomputes slot 0's actual start/len from sample_synth_trim_
    // start01_/end01_ (via ComputeSampleSynthTrimRange() above) and
    // calls SampleSynth::LoadSlot() -- the only "trim" operation that
    // exists, since LoadSlot() already takes raw pointers (see
    // sample_synth.h's own doc comment on why no new engine API was
    // needed for this). Also applies whatever Tune currently has
    // selected (chromatic_/root_note_), so a Tune change calls this too
    // rather than needing its own apply function.
    void ApplySampleSynthTrim();
    // Landing on Capture itself (not just passing through on the way
    // to some other page -- see ClearSampleSynthSlotsForFreshCapture()'s
    // own comment for why the actual slot wipe moved out of here) --
    // just resets the status line.
    void OnEnterSampleSynthCaptureGroup();
    // Resets every voice and clears slots 1-23, replacing whatever
    // Sound/Kit was loaded -- called right alongside each of the 3
    // real "slot 0 now holds something new" moments (Direct Record,
    // From Layer, Import), NOT on mere page entry/rotation through the
    // Capture/Trim/Tune group (that used to wipe a loaded Kit just by
    // rotating past it on the way elsewhere -- a real reported bug).
    void ClearSampleSynthSlotsForFreshCapture();
    void TriggerSaveSampleSynthCapture();

    // --- SampleSynth pack import (SampleSynthParamPage::Import) ---------
    // Follows the face: SMPVO imports Sounds (SOUND PACK IMPORT/); SMPDR
    // first picks Kits or Breaks (K1, B2 opens, B1 back) -- each its own
    // folder, DRUM PACK IMPORT/KITS/ and /BREAKS/, so the folder a pack
    // was dropped in is what decides, nothing is sorted automatically.
    // Then K1 picks where it goes -- a New bank (default), or an existing
    // bank of that type with room for ALL of it -- and Button2 held 800ms
    // runs TriggerSampleSynthPackImport().
    char sample_synth_pack_import_status_[48] = {};
    void TriggerSampleSynthPackImport();
    // Re-scans the waiting files and rebuilds the destination list.
    void RefreshSampleSynthImport();
    // SMPDR: true once Kits or Breaks is opened (always true on SMPVO).
    bool SampleSynthImportReady() const
    {
        return sample_synth_mode_ == SampleSynthMode::Voice || sample_synth_import_drum_open_;
    }
    bool sample_synth_import_dirty_       = true;
    bool sample_synth_import_drum_open_   = false;
    bool sample_synth_import_drum_breaks_ = false; // which one is selected/open
    int  sample_synth_import_sounds_      = 0;     // waiting, per type
    int  sample_synth_import_kits_        = 0;
    int  sample_synth_import_breaks_      = 0;
    // .aif left where drum packs went before KITS/ and BREAKS/ existed
    // (loose in DRUM PACK IMPORT/, or BREAK PACK IMPORT/) -- not imported.
    int sample_synth_import_loose_ = 0;
    struct ImportDest
    {
        int number = 0; // 0 = New bank
        int free   = 0;
    };
    ImportDest sample_synth_import_dests_[1 + PerformanceStore::kMaxSampleBanks];
    int        sample_synth_import_dest_count_  = 1;
    int        sample_synth_import_dest_cursor_ = 0;
    // The bank type being imported: Sounds, or Kits/Breaks as opened.
    PerformanceStore::SampleBank SampleSynthImportType() const
    {
        using PerformanceStore::SampleBank;
        return sample_synth_mode_ == SampleSynthMode::Voice ? SampleBank::Sounds
               : sample_synth_import_drum_breaks_           ? SampleBank::Breaks
                                                            : SampleBank::Kits;
    }
};
