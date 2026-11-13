#pragma once
#include "daisy_pod.h"
#include "dev/oled_ssd130x.h"
#include "tempo_clock.h"
#include "sequencer.h"
#include "arpeggiator.h"
#include "scenes.h"
#include "live_export.h"
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
              DexedSynth*                   dexed_b,
              EnsembleSynth*                ensemble,
              EnsembleSynth*                organ,
              const float*                  ensemble_scope_buf,
              size_t                        ensemble_scope_capacity,
              SampleSynth*                  sample_synth,
              SampleSynth*                  sample_drum,
              const float*                  sample_synth_scope_buf,
              size_t                        sample_synth_scope_capacity,
              const float*                  bypass_scope_buf,
              size_t                        bypass_scope_capacity,
              int16_t*                      sample_synth_arena,
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

    // Panic: three quick encoder clicks ask main.cpp to release every
    // note on every engine (see Update()). Returns true once per request.
    // Global:MIDI's sustain pedal polarity -- main.cpp flips every
    // incoming CC 64 when this is set (see SustainPedal() there), and
    // reports back what the pedal last did for the page's live readout:
    // -1 nothing received yet, 0 up, 1 down (polarity already applied).
    bool IsPedalReversed() const { return pedal_reversed_; }
    void SetPedalState(int state) { pedal_state_ = state; }

    bool TakePanicRequest()
    {
        bool r            = panic_requested_;
        panic_requested_ = false;
        return r;
    }

    // Power-on splash (PZD logo + GROOVEBOX). main.cpp calls this right
    // after display.Init(), before the SD card and Ui::Init(); Update()
    // then keeps it up until kSplashMs after it appeared, or until a
    // button or encoder press skips it.
    static void DrawSplash(daisy::OneBitGraphicsDisplay* display);

    // Read by main.cpp's audio callback every block.
    float GetMasterVolume() const { return master_volume_; }
    // Bypass on -- or the layer Gain page open, which monitors the input
    // through that layer's gain so it can be set by ear.
    bool  IsBypassed() const
    {
        return bypass_ || (screen_ == Screen::Layer && layer_page_ == LayerPage::Gain);
    }
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
    // Dexed Split page, read by main.cpp's MIDI routing: in Split, DXD-A
    // plays the keys below the split note and DXD-B that note and up;
    // in Layer both play every key. Each engine's octave shift (-2..+2)
    // applies in either mode.
    bool DexedSplitOn() const { return dexed_split_; }
    int  DexedSplitNote() const { return dexed_split_note_; }
    int  DexedOctave(int engine) const { return dexed_octave_[engine == 1 ? 1 : 0]; }
    // DXD-B's channel: DXD-A's while the MIDI page's Lock is on (the
    // default), its own otherwise.
    int GetDexedBMidiChannel() const
    {
        return dexed_midi_lock_ ? dexed_midi_channel_ : dexed_b_midi_channel_;
    }
    int GetGranularMidiChannel() const { return granular_midi_channel_; }
    int GetEnsembleMidiChannel() const { return ensemble_midi_channel_; }
    int GetOrganMidiChannel() const { return organ_midi_channel_; }
    int GetSampleSynthMidiChannel() const { return sample_synth_midi_channel_; }
    int GetSampleDrumMidiChannel() const { return sample_drum_midi_channel_; }
    void SetSequencer(MidiSequencer* seq) { seq_ = seq; }
    void SetArpeggiator(Arpeggiator* arp) { arp_ = arp; }
    // A note to sound for a moment (the MIDI grid's cursor), played by the
    // control interrupt: it bumps PreviewSeq() when there's a new one.
    uint32_t PreviewSeq() const { return preview_seq_; }
    void     GetPreview(int* engine, int* note, int* velocity) const
    {
        *engine   = preview_engine_;
        *note     = preview_note_;
        *velocity = preview_vel_;
    }
    // Sends a raw command to the OLED panel (see pod_oled.h) -- the screen
    // saver's dimming and switching off.
    void SetDisplayCommand(void (*fn)(uint8_t)) { display_cmd_ = fn; }
    void SetLiveExport(LiveExport* lx) { live_ = lx; }
    // The layer printing a sequencer track and the engines it records
    // (see StartSeqPrint()); -1 / 0 when nothing is printing.
    int      GetSeqPrintLayer() const { return seq_print_layer_; }
    uint32_t GetSeqPrintMask() const { return seq_print_mask_; }
    // The sequencer track the keyboard plays (and records into) right now:
    // a track selected on Home or open in its settings, else -1 (normal
    // per-engine MIDI channels).
    int GetSeqLiveTrack() const
    {
        return (screen_ == Screen::Home || screen_ == Screen::SeqTrack) ? HomeSeqTrack() : -1;
    }
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
    // DXD-B, the second Dexed engine (Global:DXD-B) -- same gate.
    bool IsDexedBEnabled() const { return dexed_b_enabled_; }
    // Same real-CPU-saving on/off gate, for the new Ensemble engine (see
    // main.cpp's AudioCallback()).
    bool IsEnsembleEnabled() const { return ensemble_enabled_; }
    bool IsOrganEnabled() const { return organ_enabled_; }
    // Screen::Ensemble's scope follows whichever engine its pages edit.
    bool EnsembleScopeIsOrgan() const { return ensemble_active_ == 1; }
    // Same real-CPU-saving on/off gate, for the two sampler engines --
    // SMPVO (Voice) and SMPDR (Drum) are separate SampleSynths now and
    // can both play at once. IsSampleSynthEnabled() is true if either is.
    bool IsSampleVoiceEnabled() const { return sample_synth_voice_enabled_; }
    bool IsSampleDrumEnabled() const { return sample_synth_drum_enabled_; }
    bool IsSampleSynthEnabled() const
    {
        return sample_synth_voice_enabled_ || sample_synth_drum_enabled_;
    }
    // Screen::SampleSynth's scope follows whichever face is showing.
    bool SampleScopeIsDrum() const { return sample_synth_mode_ == SampleSynthMode::Drum; }
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
        DexedOperator,
        // A MIDI sequencer track's settings (engine, length, clear) --
        // entered by clicking a track (M1..M8) on Home.
        SeqTrack,
        // One engine's arpeggiator -- entered from Global:Arp (encoder
        // click), which picks the engine. Pages: Setup, Pattern (or
        // Phrase), Feel, Key.
        Arp
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
        Filter,
        Drive,   // K1 Drive (0 = off), K2 Trim
        Tremolo, // K1 Depth (0 = off), K2 Rate
        Reverb,
        kCount
    };
    enum class GlobalPage
    {
        // First -- where Global opens. Entry point into Screen::Mixer
        // (the real per-engine level controls live there), same "entry
        // point only" idiom as Granular/Dexed below.
        Mixer,
        // Every channel's reverb and delay send on one page: K1 picks the
        // channel, B1/B2 point K2 at its Reverb or Delay send.
        Sends,
        Tempo,
        // Saving and the card, right after Tempo: Perf (Global:Perf), then
        // every synth engine at once -- B1 saves a new snapshot, B2 opens
        // the list (K2 scrolls, B2 loads), see PerformanceStore::
        // SynthSnapshot -- then file management (Knob1 picks a save
        // folder, Button1 tap drills in; inside, Knob1 browses its files,
        // Button1 tap goes back up, Button1 hold Duplicates, Button2 hold
        // Deletes -- see SdMgmtFolder/sd_mgmt_in_folder_), then Export.
        File,
        Snap,
        SdMgmt,
        Export,
        // The screen saver: K1 its timeout, B1 sleep now, B2 saves the
        // timeout as the power-up default (SCREEN.DAT).
        Display,
        Speed, // the project-wide transport, ahead of the shared FX pages
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
        // Every engine's MIDI channel in one list (K1 picks the engine,
        // K2 its channel), plus the sustain pedal polarity (B1 Normal /
        // B2 Reversed, saved to PEDAL.DAT on change).
        Midi,
        // Every engine's arpeggiator at a glance: K1 picks an engine, B1
        // turns its arp on/off, B2 switches it Generator/Phrase, a click
        // opens it (Screen::Arp).
        Arp,
        // Entry point into Screen::Granular (not built yet -- Stage 2 of
        // the granular rewrite). For now this page only exposes the
        // on/off toggle pulled forward from Stage 4, same idiom as Pad's
        // own toggle below.
        Granular,
        // Entry point into Screen::Dexed, plus the on/off toggle -- same
        // Button1-tap convention as Granular's own above.
        Dexed,
        // DXD-B, the second Dexed engine -- same Screen::Dexed, entered
        // with dexed_ pointing at it (see SelectDexedEngine()).
        DexedB,
        // Entry point into Screen::Ensemble, plus the on/off toggle --
        // same Button1-tap convention as Granular/Dexed above.
        Ensemble,
        // The Organ: the same Screen::Ensemble pages, entered with
        // ensemble_ pointing at the Organ engine (SelectEnsembleEngine()).
        Organ,
        // Entry point into Screen::SampleSynth, plus the on/off toggle --
        // same treatment as Granular/Dexed/Ensemble above. Two entries
        // (Voice/Drum, see Ui::SampleSynthMode's own comment), each its
        // own SampleSynth engine, sharing that one Screen's pages.
        SampleSynthVoice,
        SampleSynthDrum,
        // On/off toggle only, same treatment as Granular/Dexed above --
        // but for the whole 4-layer loop system, and a REAL stop (see
        // IsLooperEnabled()'s doc comment), not a layer's own
        // phase-locked pause. Lets someone using only Dexed/Grains skip
        // all 4 LooperLayer::Process() calls entirely for the CPU back,
        // not just silence.
        Looper,
        kCount
    };
    // Declared in this exact order because DrawSdMgmtScreen()'s own grid
    // maps (int)SdMgmtFolder straight to a row-major cell index (4
    // columns x 2 rows) -- reordering these moves which box each one
    // draws in. Never persisted anywhere (sd_mgmt_folder_ always boots
    // back to Global), so reordering is otherwise free.
    enum class SdMgmtFolder
    {
        // Performances and Global:Snap's snapshots, chosen inside the box
        // (see sd_mgmt_global_kind_).
        Global,           // row0 col0
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
        // GR-1 names and order. Grain: Grain Size + Density, with the
        // waveform, Position's span and every voice's live pointer.
        Grain,
        // Position + Spray -- where the pointers sit, and how far each
        // grain's start is scattered around its pointer.
        Position,
        // Scan (pointer speed and heading, -2x..+2x); B1 cycles Scan mode
        // Loop / One shot / Ping pong, B2 cycles each grain's Direction
        // Forward / Reverse / Random.
        Scan,
        // Rhythm pattern (B1) and its Speed (B2); no knobs. Ours, not the
        // GR-1's.
        Rhythm,
        // Loop Start + Loop End: the range the pointers scan within.
        Loop,
        // Pitch (semitones); B1 Keymap (keyboard tracking) On/Off, B2
        // Mono/Poly.
        Pitch,
        // Attack/Decay/Sustain/Release graph, same merged-page idiom as
        // Grain's Size/Fill/Gap/Scan and Pad's own ADSR page.
        ADSR,
        Filter, // Cutoff + Resonance, mode cycled by Button1; live oscilloscope
        Mix,    // Level + Pan, Gain (same layout as every engine)
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
        Algo,       // view-only diagram -- ALGORITHM is set in Advanced
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
        // Layer or Split for DXD-A/DXD-B, the split point and each
        // engine's octave shift -- one shared setting, the same page from
        // either engine (see Ui::DexedSplitOn()).
        Split,
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
        // Organ only (skipped on the Ensemble): percussion -- K1 Decay,
        // K2 Volume, B1 On/Off, B2 2nd/3rd -- and key click (K1).
        Perc,
        Click,
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

    // Screen::DexedOperator's own pages -- the full DX7 voice, named
    // with the DX7's own parameter wording. Operator pages first
    // (Button1 cycles OP1-6, shown as the real HW op number, i.e.
    // 6-dexed_op_index_), then the voice-wide pages from PitchEgRate on
    // (Button1 unused -- see IsDexedVoicePage()). Rotate stays reserved
    // for page navigation, matching every other screen. Button2 is each
    // page's own toggle, listed per page below. Every value here is a
    // byte of the 156-byte unpacked patch (byte map in dexed_synth.cpp)
    // except OP ON/OFF, which is DexedSynth::SetOpSwitch().
    enum class DexedOpParamPage
    {
        // ALGORITHM on K1 (1-32) with the routing diagram -- the one
        // place it's set; Screen::Dexed's own Algo page only shows it.
        Algo,
        Frequency,         // F COARSE / F FINE, B2 = OSC MODE RATIO/FIXED
        Detune,            // OSC DETUNE, shown -7..+7
        OutputLevel,       // OUTPUT LEVEL, B2 = OP ON/OFF
        EgRate,            // RATE 1/2, B2 -> RATE 3/4
        EgLevel,           // LEVEL 1/2, B2 -> LEVEL 3/4
        LevelScalingCurve, // L CURVE / R CURVE, B2 swaps K1 to BREAK POINT
        LevelScalingDepth, // L DEPTH / R DEPTH
        RateScaling,       // RATE SCALING 0-7
        Sensitivity,       // KEY VELOCITY 0-7 / A MOD SENS 0-3
        // --- voice-wide from here on ---
        PitchEgRate,  // PITCH EG RATE 1/2, B2 -> 3/4
        PitchEgLevel, // PITCH EG LEVEL 1/2, B2 -> 3/4
        Lfo,          // LFO SPEED / LFO DELAY, B2 cycles LFO WAVE
        LfoDepth,     // LFO PM DEPTH / LFO AM DEPTH, B2 = LFO KEY SYNC
        ModSens,      // P MOD SENS, B2 = OSC KEY SYNC
        Transpose,    // TRANSPOSE, shown as the note middle C plays
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
        LayerFilter,
        LayerDrive,
        LayerTremolo,
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
        GlobalMidi, // K1 picks an engine row (no pickup), K2 its channel
        GlobalArp,  // K1 picks an engine
        ArpSetup,   // K1 picks Rate/Tempo/Range, K2 sets it
        ArpPattern, // K1 steps, K2 jump (Generator); K1 phrase, K2 octave (Phrase)
        ArpFeel,    // K1 gate, K2 swing
        ArpKey,     // K1 key, K2 scale
        GlobalMixer, // entry point only -- see Screen::Mixer instead
        GlobalSends, // K1 channel, K2 its reverb or delay send
        GlobalSdMgmt, // browses a list directly, no pickup used -- see GlobalPage::SdMgmt
        GlobalDexed, // entry point only -- see Screen::Dexed instead
        GlobalEnsemble, // entry point only -- see Screen::Ensemble instead
        GlobalSampleSynth, // no continuous knobs -- Button1 toggle only, same as GlobalLooper
        GranularGrain,    // Grain Size + Density
        GranularPosition, // Position + Spray
        GranularRhythm,   // no knobs -- Button1/Button2 only
        GranularScan,     // Scan only
        GranularLoop,     // Loop Start + Loop End
        GranularPitch,
        GranularEnvAD,
        GranularEnvSR,
        GranularFilter,
        GranularMix,     // Level + Pan
        GranularMixGain, // Gain
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
        DexedMix, // the bus's Level + Pan
        DexedMixGain,
        // The A and B bars on the Mix page: that engine's own Level + Pan
        // into the bus (DexedSynth::SetEngineLevel01()/SetEnginePan01()).
        DexedMixA,
        DexedMixB,
        // Screen::Dexed's own new FX page -- same shape/reasoning as
        // GranularFxReverb/GranularFxDelay above.
        DexedFxReverb,
        DexedFxDelay,
        DexedFold, // Wavefolder's own Amount + Trim pair -- no sub-mode toggle
        DexedMidi, // K1 = which MIDI channel this engine listens on, K2 = MAX VOICES
        DexedSplit, // K1 = split point, K2 = octave shift of A or B (B2 picks)
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
        DexedOpAlgo, // K1 = ALGORITHM
        DexedOpFrequency,
        DexedOpDetune,
        DexedOpOutputLevel,
        DexedOpEgRate12,
        DexedOpEgRate34,
        DexedOpEgLevel12,
        DexedOpEgLevel34,
        DexedOpLevelScalingCurve,
        DexedOpBreakPoint, // LevelScalingCurve with K1 swapped to BREAK POINT
        DexedOpLevelScalingDepth,
        DexedOpRateScaling,
        DexedOpSensitivity,
        DexedVoicePitchEgRate12,
        DexedVoicePitchEgRate34,
        DexedVoicePitchEgLevel12,
        DexedVoicePitchEgLevel34,
        DexedVoiceLfo,
        DexedVoiceLfoDepth,
        DexedVoiceModSens,
        DexedVoiceTranspose,
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
        OrganPerc,    // K1 percussion Decay, K2 its Volume
        OrganClick,   // K1 key click amount
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
        SeqTrack,          // K1 engine, K2 length
        SeqQuant,          // K1 quantise, K2 swing
        SeqGrid,           // K1 step, K2 note (B2 held: edit the note)
        SeqPrint,          // K1 destination layer, K2 this track / all tracks
        SeqPatt,           // K1 pattern A..H, K2 that pattern's transpose
        HomeScenes,        // K1 scene, K2 master volume
        HomeSceneEdit,     // K1 layer/track, K2 its volume/pattern
        GlobalDisplay,     // K1 the screen saver's timeout
        SeqChain,          // K1 pattern A..H to add
        SeqArp,            // K1 the pattern whose arp B1/a click works on
        HomeSong,          // K1 step, K2 master volume
        HomeSongEdit,      // K1 the step's scene, K2 its repeats
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
    // Layer 1..4, 4 is Grains, 5 is Dexed, 6 is Ensemble, 7 is Organ, 8
    // is SMPVO (VOI), 9 is SMPDR (DRM), 10 is Bypass, 11 is Master. Kept
    // as small indexed switches rather than a polymorphic interface
    // since there are only a dozen cases and they already read/write each engine's own real
    // getters/setters directly -- no new state of its own, this is
    // purely another view onto values that already exist.
    static constexpr int kNumMixerChannels = 12;
    // Total encoder "stops" in Screen::Mixer -- the real channels above
    // plus the Scope stop (mixer_position_ == kNumMixerChannels). See
    // Screen::Mixer's own comment.
    static constexpr int kNumMixerPositions = kNumMixerChannels + 1;
    const char* MixerChannelName(int ch) const;
    float       MixerGetVolume01(int ch) const;
    float       MixerGetPan01(int ch) const; // Master (last channel) has none -- returns 0.5f
    float       MixerGetSend01(int ch) const; // Master (last channel) has none -- returns 0.f
    float       MixerGetDelaySend01(int ch) const; // likewise
    void        MixerSetDelaySend01(int ch, float v01);
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
    // The same, lying down: fills left to right, the clip cap at the
    // right-hand end -- the Overview grid's meters.
    void DrawMixerHBar(int x0, int y0, int w, int h, float v01, bool clip_lit = false);
    // Every channel's name with its live peak meter (a horizontal bar
    // under the name), 2 rows of 6 -- GlobalPage::Mixer's at-a-glance
    // summary (see DrawGlobalScreen()). `top_y` is the first name row's
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
    // Real DX7 4-rate/4-level envelope (operator or PITCH EG) --
    // genuinely different from DrawAdsrShape() above: it can rise and
    // fall repeatedly, and never has to start or end at zero.
    // Drawn to match the DX7's own picture: the line starts at LEVEL
    // 4, each RATE is a slope between level vertices (higher rate =
    // steeper, and a bigger level move takes longer), LEVEL 3 holds
    // until a dotted key-release marker, then RATE 4 falls back to
    // LEVEL 4. hl_rates/hl_levels are bitmasks (bit n = RATE/LEVEL n+1)
    // of what the knobs are editing -- reversed slope numbers and filled
    // level squares. center_line draws a
    // dotted line at level 50, for PITCH EG.
    void DrawDx7EnvelopeShape(int top, int bottom, const uint8_t rates[4], const uint8_t levels[4],
                              uint8_t hl_rates = 0, uint8_t hl_levels = 0,
                              bool center_line = false);
    // Keyboard level scaling across the whole keyboard: the level drop
    // left/right of BREAK POINT (marked) for the given operator.
    void DrawLevelScalingGraph(int top, int bottom, int base);
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
                                const float* pointers01,
                                int          num_pointers,
                                bool         has_source,
                                float        grain_size01 = 0.f,
                                float        range_start01 = -1.f,
                                float        range_end01   = -1.f);
    // DrawGranularWaveform() with the live engine's own peaks, Position,
    // grain span and voice pointers -- every Grains page uses this.
    void DrawGranularWaveformLive(float range_start01 = -1.f, float range_end01 = -1.f);
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
    // Global:Snap -- see GlobalPage::Snap.
    void RefreshSnapSlots();
    // Overwrites snap_loaded_slot_ unless force_new (or nothing's loaded).
    void TriggerSaveSnapshot(bool force_new);
    void TriggerLoadSnapshot(int slot);
    void DrawSnapScreen();
    void CaptureSynthSnapshot(PerformanceStore::SynthSnapshot* out);
    // The name Global:Snap's grid shows for engine 0-3 (Grains, Dexed,
    // Ensemble, Sampler) as it is right now.
    void LiveSnapEngineName(int engine, char* out, size_t out_size);
    // The SMPVO/SMPDR User preset "apply" steps, shared by their own
    // Load and by snapshot loads. An empty filename/kit_ref keeps
    // whatever sound is there and applies the settings only. Drum
    // returns false if its Kit is missing.
    void ApplySampleSynthVoiceUserPreset(PerformanceStore::SampleSynthVoiceUserPreset& preset);
    bool ApplySampleSynthDrumUserPreset(const PerformanceStore::SampleSynthDrumUserPreset& preset);
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
                   || (SdMgmtDrumUserBox() && sd_mgmt_drum_kind_ < 0)
                   || (SdMgmtGlobalBox() && sd_mgmt_global_kind_ < 0)
                   || (SdMgmtEnsembleBox() && sd_mgmt_ens_kind_ < 0));
    }
    // GLOBAL box: Performances and Global:Snap's snapshots, picked first.
    bool SdMgmtGlobalBox() const { return sd_mgmt_folder_ == SdMgmtFolder::Global; }
    bool SdMgmtSnapshots() const { return SdMgmtGlobalBox() && sd_mgmt_global_kind_ == 1; }
    // ENS/ORG box: the Ensemble's and the Organ's User saves,
    // picked first, the same way as GLOBAL's Perf/Snapshots.
    bool SdMgmtEnsembleBox() const { return sd_mgmt_folder_ == SdMgmtFolder::EnsemblePresets; }
    // The file prefix SdMgmt's status line names an item by (PERF3,
    // SNAP3, DEXED12...).
    const char* SdMgmtItemPrefix() const
    {
        if(SdMgmtGlobalBox())
            return SdMgmtSnapshots() ? "SNAP" : "PERF";
        if(SdMgmtEnsembleBox())
            return EnsembleSlotPrefix(); // ENSM / ORGN
        return SdMgmtFolderShortLabel(sd_mgmt_folder_);
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
    // Before a Dexed User save: if another User save already has this
    // patch's name, renames it "NAME 2", "NAME 3"... (cut to fit 10
    // characters). Returns true if it renamed. Reads the card.
    bool NumberDexedSaveName(DexedSynth::DexedPresetData* p, int target_slot);
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
    // Every engine's (and the Looper's) Global entry page: title,
    // Enabled/Disabled, the page's graphic (ui_graphics.h), the encoder
    // hint and the B1 Toggle On/Off footer.
    void DrawGlobalEntryPage(const char*    title,
                              bool           enabled,
                              const uint8_t* icon,
                              const char*    status_right = "",
                              const char*    b2_label     = "");
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
    // Global:Tempo's metronome, top-left at (x, y), 19x20. swing -1/0/+1
    // leans the arm left/upright/right.
    void DrawMetronome(int x, int y, int swing);
    void DrawBeatIndicator(int x, int y, int dot_size);
    // A Home box's transport state, drawn beside its name (top left at
    // x, y, 7px tall like the name's letters): - empty, a triangle playing,
    // two bars paused, a filled circle recording, a ring counting in.
    enum class StateIcon : uint8_t
    {
        Empty,
        Play,
        Pause,
        Rec,
        CountIn
    };
    void DrawStateIcon(int x, int y, StateIcon icon);

    LooperLayer& Cur() { return layers_[cursor_layer_]; }
    // --- MIDI sequencer (Home's M1..M8) -----------------------------------
    // home_slot_ is Home's selection across all 13: 0..3 the audio layers
    // (cursor_layer_ follows it), 4..11 the sequencer tracks M1..M8, four
    // to a page, then the Scenes page.
    static constexpr int kHomeSceneSlot = 4 + MidiSequencer::kTracks;
    static constexpr int kHomeSongSlot  = kHomeSceneSlot + 1;
    static constexpr int kHomeSlots     = kHomeSongSlot + 1;
    int  home_slot_ = 0;
    // --- Scenes (Home's last page, see scenes.h) ---------------------------
    // K1 picks one, B1 tap launches it (on the loop's next start; again
    // cancels), B1 hold saves what's playing into it, B2 hold clears it.
    Scene scenes_[kScenes];
    int   scene_cursor_  = 0;
    int   scene_playing_ = -1; // the last one launched or saved
    int   scene_next_    = -1; // launched, waiting for the loop's start
    bool  HomeScenes() const { return home_slot_ == kHomeSceneSlot; }
    void  SaveScene(int i);
    void  LaunchScene(int i);
    void  QueueScene(int i); // switch to scene i (kScenes = all off) at the loop's next start
    void  UpdateScenes();             // each Update(): a launch taking effect
    void  DrawHomeScenePage();
    // Editing a scene in place (encoder click on the Scenes page): a cell
    // per layer and track -- K1 picks one, K2 sets the layer's volume or
    // the track's pattern (Off, A..H), B2 turns it on/off. An empty scene
    // starts as what's playing. Edits to the scene playing are heard at once.
    int   scene_edit_      = -1; // the scene being edited, -1 = none
    int   scene_edit_cell_ = 0;  // 0..3 L1..L4, 4..11 M1..M8
    static constexpr int kSceneCells = Scene::kLayers + MidiSequencer::kTracks;
    void  SceneEditValue(float k2);
    void  SceneEditToggle();
    void  DrawSceneEditPage();
    // --- Song (Home's page after Scenes) -----------------------------------
    // K1 picks a step (the one past the end adds), encoder click toggles
    // Edit (K1 its scene, K2 its repeats). B1 tap plays from the chosen
    // step / stops; B1 hold records -- launch scenes on the Scenes page and
    // each becomes a step, for the loops it played. B2 tap inserts a copy
    // of the step, B2 hold deletes it.
    Song     song_;
    int      song_cursor_    = 0;
    bool     song_edit_      = false;
    bool     song_playing_   = false;
    bool     song_waiting_   = false; // its first scene is launched, not started yet
    bool     song_queued_    = false; // the next step's scene is launched
    int      song_step_      = 0;
    uint32_t song_step_loop_ = 0;     // g_loop_starts where song_step_ began
    bool     song_rec_       = false;
    bool     song_rec_armed_ = false; // waiting for the loop's start to begin
    uint32_t song_rec_loop_  = 0;     // where the step being recorded began
    // The list shows 3 columns of 5; while the song plays the view follows
    // the step playing, except for a few seconds after K1 moves the cursor.
    int      song_view_col_  = 0;     // first column shown
    uint32_t song_k1_ms_     = 0;     // when K1 last moved the cursor
    bool     HomeSong() const { return home_slot_ == kHomeSongSlot; }
    void     StartSong(int from);
    void     StopSong();
    void     StartSongRecord();
    void     StopSongRecord();
    void     UpdateSong(); // each Update()
    void     DrawHomeSongPage();
    // --- Global:Export's Live mode (see live_export.h) ---------------------
    LiveExport* live_         = nullptr;
    // K1: Fast (the layers, offline) / Live (the master, as it plays) /
    // Song (the song from the Song page's step, however long it runs).
    static constexpr int kExportFast = 0, kExportLive = 1, kExportSong = 2;
    int         export_mode_  = kExportFast;
    int         export_loops_ = 1;     // K2 in Live: loops to record, 1..8
    bool        live_open_    = false; // a live export is running (armed, recording or saving)
    bool        live_44k_     = false; // ... for MicroDexed (44.1kHz, custom/)
    int         LiveExportMaxLoops() const; // what fits in the memory it records into
    char        live_name_[12] = {};
    void        StartLiveExport(bool for_microdexed);
    void        StopLiveExport();
    void        UpdateLiveExport(); // each Update(): drain the ring to the card
    // Song export: the file opens first and fills a chunk at a time.
    bool        WriteLiveFrames(uint32_t frames); // audio paused by the caller
    void        StartSongExport(bool for_microdexed);
    void        UpdateSongExport();
    uint32_t    song_export_written_ = 0; // frames already in the file
    bool        song_tail_           = false; // the song's over: recording until quiet
    uint32_t    song_tail_start_     = 0;     // total frames when it ended
    uint32_t    song_quiet_from_     = 0;     // total frames when it went quiet (0 = not)
    bool        LiveExportBusy() const { return live_open_; }
    int  HomeSeqTrack() const
    {
        return home_slot_ >= 4 && home_slot_ < kHomeSceneSlot ? home_slot_ - 4 : -1;
    }
    MidiSequencer* seq_ = nullptr;
    // --- Arpeggiators (Global:Arp, Screen::Arp; see arpeggiator.h) --------
    Arpeggiator* arp_        = nullptr;
    int          arp_engine_ = 1; // main.cpp's SustainEngine order; DXD-A
    int          arp_page_   = 0; // Screen::Arp: 0 Setup, 1 Pattern, 2 Feel, 3 Key
    int          arp_setup_sel_ = 0; // Setup's K2: 0 Rate, 1 Tempo, 2 Range
    // Screen::Arp edits engine arp_engine_'s arp, or -- entered from a
    // track's Arp page -- track arp_track_'s pattern arp_pattern_'s.
    int          arp_track_   = -1;
    int          arp_pattern_ = 0;
    Arpeggiator::Settings& ArpEdit();
    int                    ArpInst() const;
    bool                   ArpLive() const; // what's playing is what's being edited
    static constexpr int kArpPages = 4;
    static const char* ArpEngineName(int e, bool short_name);
    void DrawArpGlobalPage();
    void DrawArpScreen();
    void DrawArpPattern(const Arpeggiator::Settings& s);
    void DrawStarToggle(bool left, const char* pair, bool second);
    void Preview(int engine, int note, int velocity);
    volatile uint32_t preview_seq_    = 0;
    volatile int      preview_engine_ = 0, preview_note_ = 60, preview_vel_ = 100;
    void HandleArpVariationButton(daisy::Switch& b, bool released, int dir, bool& repeating);
    float arp_var_next_ms_ = 0.f; // Var -/+ held: when the next repeat is due
    int   arp_var_repeats_ = 0;
    void DrawArpPhrase(const Arpeggiator::Settings& s);
    // --- Reverb and delay pictures ----------------------------------------
    // The reverb as a decay graph (the dry hit, the pre-delay gap, early
    // reflections, the tail -- its length Size, height PreFilter, texture
    // Decay Diffusion, thinning towards the end Damping); the delay as an
    // echo timeline (spacing Time, shrinking by Feedback, ping-pong above/
    // below the line, thinner repeats for a darker Tone, a haze after each
    // for Dly-Rev). Both from the shared settings, scaled by send01.
    enum class RevMark : uint8_t
    {
        None,
        Size,
        PreDelay
    };
    void DrawReverbGraph(int x0, int x1, int ytop, int ybot, float send01, RevMark mark);
    void DrawDelayGraph(int x0, int x1, int ymid, int amp, float send01, bool beat_ticks);
    // An FX page's body: reverb and delay side by side at this send each,
    // the knobs' target (0 reverb, 1 delay, -1 none) highlighted.
    void DrawFxSends(float rev01, float dly01, int target);
    // A filter's response (bass left, treble right): its mode's shape,
    // bending at the cutoff, peaking with resonance; flat when it's off.
    void DrawFilterCurve(FilterMode mode, float cutoff01, float res01);
    // The layer pages' pictures: the live input against the clip lines
    // (Gain), a clean wave and the driven one (Drive), the level over
    // time (Tremolo).
    void DrawGainScope();
    void DrawDriveCurve(float drive01, float trim01);
    void DrawTremoloCurve(float depth01, float rate01);
    // --- Global:Sends ------------------------------------------------------
    // The Mixer's channels 0..10 (the layers, every engine, Bypass).
    static constexpr int kSendChannels = 11;
    int  sends_ch_           = 0;
    bool sends_target_delay_ = false;
    void DrawSendsPage();
    // --- Screen saver (Global:Display) -------------------------------------
    // With no knob, button or encoder touched for the timeout, the panel
    // dims; a minute later it switches off (OLEDs burn in). The first
    // touch only wakes it -- that press, turn or click does nothing else,
    // and the knobs have to pick their values up again. MIDI doesn't wake it.
    enum class Saver : uint8_t
    {
        Awake,
        Dim,
        Off
    };
    static constexpr int kSaverOptions = 6; // Off, 1, 2, 5, 10, 30 minutes
    static int SaverMinutes(int option);
    void       (*display_cmd_)(uint8_t) = nullptr;
    int        saver_option_  = 3; // 5 minutes until SCREEN.DAT says otherwise
    int        saver_default_ = 3; // what SCREEN.DAT holds
    Saver      saver_         = Saver::Awake;
    uint32_t   saver_touch_ms_ = 0;    // the last touch
    float      saver_k1_ = -1.f, saver_k2_ = -1.f; // knobs at the last touch
    bool       saver_swallow_ = false; // the waking touch isn't over yet
    char       saver_status_[24] = {};
    // Returns true while the controls should be ignored (asleep, or the
    // touch that woke it still going).
    bool       UpdateSaver(const UiControlEvents& ev);
    void       SetSaver(Saver s);
    void       DrawDisplayPage();
    // A track's pages (Screen::SeqTrack), rotated through with the
    // encoder: Track (engine, length), Quant (quantise, swing), then one
    // Grid page per bar of the track.
    static constexpr int kSeqPageTrack = 0, kSeqPagePatt = 1, kSeqPageChain = 2,
                         kSeqPageArp = 3, kSeqPageQuant = 4, kSeqPagePrint = 5, kSeqPageGrid = 6;
    void DrawSeqChainPage(int t);
    int  seq_patt_cursor_ = 0; // the Patt page's K1 choice
    void DrawSeqPattPage(int t);
    void DrawSeqArpPage(int t);
    // --- Printing a track to a layer (the Print page) ----------------------
    // That layer records only the track's engine (main.cpp reads
    // GetSeqPrintLayer()/Mask()); when its pass is done the tracks it
    // printed pause, so they aren't heard twice.
    int               seq_print_dest_     = 0;  // the Print page's chosen layer
    bool              seq_print_all_      = false; // K2: every track with notes, not just this one's engine
    volatile int      seq_print_layer_    = -1; // printing into this layer (-1 = not)
    volatile uint32_t seq_print_mask_     = 0;  // engines being printed (1 << engine)
    uint32_t          seq_print_tracks_   = 0;  // the tracks being printed (1 << track)
    bool              seq_print_recorded_ = false; // the layer got as far as recording
    char              seq_print_status_[24] = {};
    bool     SeqPrintFits(int t) const; // the track is no longer than the loop
    // The tracks a print would take -- this one's engine, or (All) every
    // track with notes -- as a bitmask of tracks and of their engines.
    uint32_t SeqPrintTracks(int t, uint32_t* engines) const;
    void StartSeqPrint(int t);
    // --- A track's arp on its grid, and printed into notes -------------
    // What the playing pattern's arp plays, worked out offline (see
    // Arpeggiator::Render()); cached until the notes or the arp change.
    // Returns how many; 0 if its arp is off.
    int  RenderTrackArp(int t, const Arpeggiator::RenderedNote** out);
    // Print page, K1 past the layers: the arp's notes into the track's next
    // empty pattern (its arp off), switched to at the pattern's end.
    void PrintArpToNotes(int t);
    uint32_t ghost_sig_   = 0; // RenderTrackArp()'s cache key (0 = none)
    int      ghost_count_ = 0;
    void UpdateSeqPrint();           // each Update(): finish a print
    void DrawSeqPrintPage(int t);
    int  seq_page_ = kSeqPageTrack;
    int  SeqGridBars(int t) const; // grid pages for track t
    // Ticks one grid page shows: a bar, or half of one when a bar would
    // be more than 24 steps (1/32, 1/32T) -- so a cell is never under 4px.
    static uint32_t SeqGridPageTicks(uint32_t step);
    // The grid (see DrawSeqGridPage()): its cursor -- a step in the bar
    // shown and a note -- and the first note row on screen.
    struct SeqGridView
    {
        uint32_t len_ticks;   // the track's length
        uint32_t step;        // ticks per cell
        uint32_t page_start;  // first tick of the page shown
        int      cells;       // cells on this page (fewer on a short track)
        int      cells_per_page; // on a full page -- sets the cell width
        bool     drum;        // pads C2 upward, top down; else pitch, high at top
        int      note_lo, note_hi;
    };
    SeqGridView SeqGridGeometry(int t) const;
    int  grid_col_  = 0; // -1 = the row itself (K1 fully left): B2 edits all its notes
    int  grid_note_ = 36;
    int  grid_top_  = 36; // drum: first row; instrument: the top (highest) row
    // B2 held on a note: K1/K2 edit it (vel or length / roll) across their
    // whole range, each picked up -- it starts following once turned
    // through the note's current value, so nothing jumps on the press.
    bool  grid_edit_      = false;
    bool  grid_edit_note_ = false; // the hold landed on a note
    bool  grid_edit_k1_on_ = false, grid_edit_k2_on_ = false;     // picked up yet
    bool  grid_edit_k1_below_ = false, grid_edit_k2_below_ = false; // started below the value
    int   grid_edit_v1_ref_ = 0, grid_edit_roll_ref_ = 0;
    int   grid_edit_v1_ = 0, grid_edit_roll_ = 0; // live values, for the footer
    bool  SeqFindNoteAt(int t, const SeqGridView& v, int col, int note, int* vel, int* roll,
                        uint32_t* len) const;
    void  SeqGridToggle(int t);
    void  SeqGridKnobs(int t, float k1, float k2);
    void  DrawSeqGridPage(int t);
    float SeqEditK1Target(const SeqGridView& v) const;
    void DrawHomeSeqPage();
    void        SeqRecord(int t); // B1 hold
    void        SeqClearAll();    // every track Empty (New, and before a Load)
    // A Synth Snapshot's engines applied (Global:Snap load, and a
    // Performance load). true = the Sampler's kit is missing.
    bool        ApplySynthSnapshot(PerformanceStore::SynthSnapshot& snap, size_t audio_len);
    void        DrawSeqQuantPage(int t);
    const char* SeqB1Label(int t) const;
    void DrawSeqTrackScreen();

    daisy::DaisyPod*               pod_     = nullptr;
    daisy::OneBitGraphicsDisplay*  disp_    = nullptr;
    TempoClock*                    tempo_   = nullptr;
    LooperLayer*                   layers_  = nullptr;
    int                            num_layers_ = 0;

    Screen     screen_      = Screen::Home;
    int        cursor_layer_ = 0;
    LayerPage  layer_page_  = LayerPage::Status;
    GlobalPage global_page_ = GlobalPage::Mixer;

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
    // -1 = Omni, 0..15 = Ch1..16. Every engine starts on Ch1 (SMPDR on
    // Ch10) until startup defaults say otherwise -- see
    // GetDexedMidiChannel()/GetGranularMidiChannel()/GetSampleSynthMidiChannel().
    // A user preset's saved MIDI channel (-1 = Omni) replaces *channel;
    // -2 ("not stored": factory sounds, files saved before presets
    // carried a channel) leaves it alone.
    static void ApplyPresetMidiChannel(int32_t saved, int* channel)
    {
        if(saved >= -1 && saved <= 15)
            *channel = (int)saved;
    }
    int dexed_midi_channel_        = 0; // DXD-A's, and B's too while locked
    int dexed_b_midi_channel_      = 0; // DXD-B's own, used while unlocked
    bool dexed_midi_lock_          = true; // MIDI page B1: B follows A's channel
    // Split page (see DexedSplitOn()). The point is limited to the keys
    // the page's keyboard draws.
    static constexpr int kDexedSplitLowNote  = 25; // C#1, so A always gets at least C1
    static constexpr int kDexedSplitHighNote = 77; // F5, the drawn keyboard's top key
    bool dexed_split_          = false;
    int  dexed_split_note_     = 60; // middle C, shown as C3
    int  dexed_octave_[2]      = {0, 0};
    bool dexed_split_oct_b_    = false; // B2: K2 shifts B rather than A
    void DrawDexedSplitPage();
    // The channel the Dexed pages show and set: the shared one while
    // locked, otherwise the edited engine's own.
    int* DexedActiveChannelPtr()
    {
        return (dexed_midi_lock_ || dexed_active_ == 0) ? &dexed_midi_channel_
                                                        : &dexed_b_midi_channel_;
    }
    int DexedActiveChannel() const
    {
        return (dexed_midi_lock_ || dexed_active_ == 0) ? dexed_midi_channel_
                                                        : dexed_b_midi_channel_;
    }
    int granular_midi_channel_     = 0;
    int ensemble_midi_channel_     = 0;
    int organ_midi_channel_        = 0;
    int sample_synth_midi_channel_ = 0; // SMPVO
    int sample_drum_midi_channel_  = 9;  // SMPDR -- Ch10, the usual drum channel
    // The showing face's own channel -- what the sampler's MIDI page,
    // User presets and snapshots read/write for "this face".
    int* SampleMidiChannel()
    {
        return sample_synth_mode_ == SampleSynthMode::Drum ? &sample_drum_midi_channel_
                                                           : &sample_synth_midi_channel_;
    }
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
    // Dexed Mix page: which bar K1/K2 set -- 0 = DXD-A, 1 = DXD-B, 2 = the
    // shared bus. Same whichever engine the page was entered from.
    int  dexed_mix_sel_                = 0;
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
    // Global:Looper B2: whether looper_enabled_ starts true at power-on
    // (PerformanceStore::LoadLooperDefault(), read in ApplyStartupDefaults()).
    bool looper_default_on_ = true;
    // Global:MIDI (see IsPedalReversed()).
    bool         pedal_reversed_ = false;
    volatile int pedal_state_    = -1;
    char         midi_status_[20] = "";
    int          midi_row_         = 0; // Global:MIDI's selected engine row
    static constexpr int kMidiRows = 7; // Grains, DXD-A, DXD-B, Ensemble, Organ, SMPVO, SMPDR
    // Global:Snap and Global:MIDI show one column per engine, 7 x 18px
    // across the screen, between the title rule (y=9) and the footer
    // divider (y=46). kEngineColLabels are the short names on top.
    static constexpr int kSnapRows      = 7;
    static constexpr int kEngineColW    = 18;
    static constexpr int kEngineCol0X   = 1;
    static constexpr const char* kEngineColLabels[kSnapRows]
        = {"GR", "DXA", "DXB", "ENS", "ORG", "SMV", "SMD"};
    // Global:Snap lists the engines in two columns of rows -- short name,
    // on/off dot, sound name -- and a name longer than its kSnapNameW
    // scrolls sideways: kSnapNameHoldMs still at each end, a pixel every
    // kSnapNameStepMs in between. Restarted (t0) whenever the snapshot
    // shown changes.
    static constexpr int kSnapColW  = 64;
    static constexpr int kSnapNameX = 20; // within a column
    static constexpr int kSnapNameW = kSnapColW - kSnapNameX - 2;
    static constexpr uint32_t kSnapNameHoldMs = 1000;
    static constexpr uint32_t kSnapNameStepMs = 60;
    uint32_t snap_name_scroll_t0_  = 0;
    int      snap_name_scroll_key_ = -2;
    int      SnapNameScroll(int name_len) const;
    int*         MidiRowChannel(int row);
    // Each engine's own channel in Global:MIDI's column order -- DXD-B's
    // own even while it's locked to A -- for its Hold=Default
    // (PerformanceStore::SaveMidiDefaults()) and the power-on load.
    int*         MidiDefaultChannel(int engine);
    void         TriggerSaveMidiDefaults();
    static float MidiChannelKnobRaw(int ch); // bucket midpoint, Omni..16
    void         SetPedalReversed(bool reversed);
    void         DrawMidiScreen();
    char looper_status_[24] = {}; // last Default save result, if it failed
    // Same false default as Granular's own now that Global:Dexed's real
    // on/off toggle exists (was temporarily true during Phase 4-6, back
    // when there was no UI toggle yet and defaulting off would have made
    // Dexed's own real MIDI dispatch unreachable/unheard).
    bool dexed_enabled_    = false;
    bool dexed_b_enabled_  = false; // DXD-B, see IsDexedBEnabled()
    // Same false default as Dexed's own -- see IsEnsembleEnabled().
    bool ensemble_enabled_ = false;
    bool organ_enabled_    = false;
    // Same false default as Dexed/Granular/Ensemble's own. One per
    // sampler engine (Global:SMPVO/Global:SMPDR's own Button1 toggle);
    // both can be on at once. See SampleSynthMode's own comment.
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
    // click). Each face is its own engine with its own arena, so this
    // only repoints the pages at the other engine and swaps the per-face
    // UI state (ParkSampleFace()) -- nothing is reloaded.
    void SwitchSampleSynthMode(SampleSynthMode target);

    uint32_t draw_counter_ = 0; // throttles the (slow, blocking-I2C) OLED redraw
    // Power-on splash, see DrawSplash(). Long enough to read, short
    // enough not to feel like waiting on every power-cycle.
    static constexpr uint32_t kSplashMs = 1500;
    bool                      splash_active_ = true;
    // Triple-click panic, see TakePanicRequest(). The first click of a
    // run notes the screen it started on; the third puts it back there,
    // so the two clicks that navigated on the way are undone.
    static constexpr uint32_t kPanicWindowMs = 900;
    static constexpr uint32_t kPanicShowMs   = 1200;
    int      panic_clicks_          = 0;
    uint32_t panic_first_click_ms_  = 0;
    Screen   panic_return_screen_   = Screen::Home;
    bool     panic_requested_       = false;
    uint32_t panic_shown_ms_        = 0; // when "ALL NOTES OFF" went up, 0 = not showing

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
    // Global:Snap's list -- same shape as the File page's above, no "New".
    int  snap_slots_[PerformanceStore::kMaxSynthSnapshots] = {};
    int  snap_slot_count_  = 0;
    int  snap_cursor_      = 0;
    bool snap_slots_dirty_ = true;
    int  snap_loaded_slot_ = -1;
    // The browsed snapshot's settings (no audio), read once per slot.
    int  snap_browse_slot_ = -1;
    bool snap_browse_ok_   = false;
    char snap_status_[32]  = {};

    // --- SD MGMT (Global:SdMgmt page) ------------------------------------
    // Reuses file_slots_/granular_preset_user_slots_ (and their own dirty
    // flags/Refresh*() calls) as the file list for whichever folder is
    // selected -- no separate copy of the same scan.
    // Delete gets a longer hold than every other confirm gesture in this
    // project (800ms elsewhere) -- unlike an overwrite or a wipe, there's
    // no "just re-save/re-load" undo for it.
    static constexpr float kSdMgmtDeleteHoldMs = 1500.f;
    SdMgmtFolder sd_mgmt_folder_    = SdMgmtFolder::Global;
    bool         sd_mgmt_in_folder_ = false; // false = picking a folder, true = browsing its files
    int          sd_mgmt_cursor_    = 0;     // item list cursor (K2)
    int          sd_mgmt_list_cursor_ = 0;   // folder-list cursor (K1): Sampler/Grains, banks
    int          sd_mgmt_capt_kind_   = -1;  // CAPT box: -1 = choosing, 0 = Sampler, 1 = Grains
    int          sd_mgmt_drum_kind_   = -1;  // SMPDR box: -1 = choosing, 0 = Kits, 1 = Breaks
    int          sd_mgmt_global_kind_ = -1;  // GLOBAL box: -1 = choosing, 0 = Perf, 1 = Snapshots
    int          sd_mgmt_ens_kind_    = -1;  // ENS/ORG box: -1 = choosing, 0 = Ensemble, 1 = Organ
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
    // false = knobs control Attack+Decay, true = Sustain+Release -- see
    // GranularParamPage::ADSR's Button1/Button2 handling.
    bool granular_adsr_target_sr_ = false;
    // false = knobs control Level+Pan, true = Gain -- see
    // GranularParamPage::Mix's Button1/Button2 handling.
    bool granular_mix_target_gain_ = false;
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
    // The engine the Dexed screens are editing: DXD-A or DXD-B, set by
    // which Global page you clicked in from (SelectDexedEngine()). Every
    // Dexed page works through this one pointer; the Mix/FX/Filter/Fold
    // pages reach the shared bus through it either way.
    DexedSynth*    dexed_            = nullptr;
    DexedSynth*    dexed_engines_[2] = {};
    int            dexed_active_     = 0; // 0 = A, 1 = B
    // The per-engine half of the Dexed UI state, parked here for the
    // engine not being edited (see SelectDexedEngine()). Browse cursors
    // stay shared -- handy for auditioning the same folder on both.
    int  dexed_parked_slot_     = 1;
    void SelectDexedEngine(int engine);
    // Re-seed the knob pickups for the current context, for when what the
    // knobs mean changes without the context itself changing.
    void ResyncKnobs();
    // Dexed Mix page bars: an engine's Level + Pan, and its Pan as text.
    void DrawDexedMixColumn(int cx, const char* name, bool selected, float level01, float pan01);
    static void FormatPan(float pan01, char* out, size_t out_size);
    // Polyphony: up to kMaxVoices for one engine alone, and at most
    // kDexedPairVoices between the two while both are on.
    static constexpr int kDexedPairVoices = 24;
    int  DexedVoiceRoom(int engine) const;
    // After `keep` was set or switched on: trims the other engine so the
    // pair stays within kDexedPairVoices.
    void ApplyDexedVoiceCeiling(int keep);
    // A preset load brings the shared bus's Mix/FX/Filter/Fold with it
    // only while the other engine is off -- otherwise picking B's sound
    // would reset what A is playing through.
    bool DexedApplyBus() const
    {
        return !(dexed_active_ == 0 ? dexed_b_enabled_ : dexed_enabled_);
    }
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
    int                dexed_op_index_ = 5; // starts on OP1
    DexedOpParamPage   dexed_op_page_  = DexedOpParamPage::Algo;
    // Button2 pair toggles (false = 1/2, true = 3/4) for the EG pages,
    // same idiom as granular_adsr_target_sr_ -- independent bools so a
    // page's own toggle state doesn't reset just from navigating away.
    bool dexed_op_egrate_target34_     = false;
    bool dexed_op_eglevel_target34_    = false;
    bool dexed_pitch_egrate_target34_  = false;
    bool dexed_pitch_eglevel_target34_ = false;
    // LevelScalingCurve's Button2: K1 = BREAK POINT instead of L CURVE.
    bool dexed_op_bp_target_ = false;
    // Which patch byte each knob edits on the current Advanced page
    // context (-1 = none), with its 0..max range -- one table shared by
    // pickup seeding, ApplyKnobs() and nothing else, so a page's knob
    // mapping lives in exactly one place.
    struct DexedEditByte
    {
        int     offset;
        uint8_t max;
    };
    static bool IsDexedVoicePage(DexedOpParamPage p)
    {
        return (int)p >= (int)DexedOpParamPage::PitchEgRate;
    }
    void DexedEditBytesFor(KnobContext ctx, DexedEditByte* k1, DexedEditByte* k2) const;
    // Screen::DexedOperator's Button2 -- each page's own toggle.
    void OnDexedOperatorButton2();
    void DrawDexedOperatorScreen();
    void DrawEnsembleScreen();

    // --- Ensemble engine (Screen::Ensemble) ------------------------------
    EnsembleSynth*    ensemble_            = nullptr; // the engine the pages edit
    // [0] Ensemble (strings), [1] Organ -- same split as dexed_engines_:
    // the pages edit ensemble_engines_[ensemble_active_], and the other
    // engine's loaded slot and unsaved baseline wait in the parked_ pair.
    EnsembleSynth* ensemble_engines_[2]    = {};
    int            ensemble_active_        = 0;
    int            ensemble_parked_slot_   = 0;
    void           SelectEnsembleEngine(int engine);
    int*           EnsembleActiveChannelPtr()
    {
        return ensemble_active_ == 1 ? &organ_midi_channel_ : &ensemble_midi_channel_;
    }
    // "ENSM"/"ORGN" -- the User slot prefix of the engine being edited.
    const char* EnsembleSlotPrefix() const { return ensemble_active_ == 1 ? "ORGN" : "ENSM"; }
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
    // Both sampler engines -- [0] Voice (SMPVO), [1] Drum (SMPDR) --
    // and their own 16-bit SDRAM arenas (see main.cpp's own
    // g_sample_synth_arena doc comment). sample_synth_ and
    // sample_synth_arena_l_/r_ point at the face that's showing, so
    // every page edits that one; SelectSampleFace() repoints
    // them. Owned by main.cpp, just pointed at here.
    SampleSynth* sample_synth_engines_[2]   = {};
    int16_t*     sample_synth_arenas_[2][2] = {};
    int16_t*     sample_synth_arena_l_        = nullptr;
    int16_t*     sample_synth_arena_r_        = nullptr;
    size_t       sample_synth_arena_capacity_ = 0;
    // The pack importer's float decode scratch: the same SDRAM block
    // as both arenas, viewed as two float buffers (so an import wipes
    // both engines' loaded audio -- see TriggerSampleSynthPackImport()).
    float* sample_synth_scratch_l_ = nullptr;
    float* sample_synth_scratch_r_ = nullptr;
    // What's loaded into the face that ISN'T showing -- the same fields
    // as the live sample_synth_loaded_*/kit_real_*/setup_* members, so
    // each engine keeps its own. Swapped by ParkSampleFace().
    struct SampleFaceState
    {
        char loaded_kit_ref[PerformanceStore::kMaxSampleSynthKitRefLen]          = {};
        char loaded_kit_name[PerformanceStore::kMaxSampleSynthKitNameLen + 1]    = {};
        char kit_status[24]                                                      = {};
        char loaded_sound_name[PerformanceStore::kMaxSampleSynthWavNameLen + 1]  = {};
        char loaded_sound_category[12]                                           = {};
        PerformanceStore::SampleSynthKitSlot real_slots[PerformanceStore::kMaxSampleSynthSlots];
        int32_t real_pad_len[PerformanceStore::kMaxSampleSynthSlots] = {};
        int     real_pad_count                                      = 0;
        size_t  real_total_len                                      = 0;
        int8_t  setup_tune_semis                                    = 0;
        int     setup_pending_slices                                = 16;
        int     setup_applied_slices                                = 0;
    };
    SampleFaceState sample_parked_face_;
    void            ParkSampleFace();
    // Reloads one face's own Kit/Sound from SD into its arena (after the
    // pack importer has used both arenas as scratch).
    void ReloadSampleFace(SampleSynthMode face);
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
    // Before a sample-file load's retry `attempt` (0 = first try: none).
    static void SampleLoadRetryPause(int attempt);
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
    // SwitchSampleSynthMode()'s engine/per-face-state half, without its
    // browsing resets -- for code that flips faces just to reach the
    // other engine (snapshots, reloads) and flips straight back.
    void         SelectSampleFace(SampleSynthMode target);
    bool         SampleSynthUnsaved() const;
    EditSnapshot sample_synth_voice_baseline_ = {};
    EditSnapshot sample_synth_drum_baseline_  = {};
    EditSnapshot EnsembleSnapshot() const { return EnsembleSnapshotFor(ensemble_active_); }
    EditSnapshot EnsembleSnapshotFor(int engine) const;
    void         MarkEnsembleClean() { ensemble_baseline_ = EnsembleSnapshot(); }
    bool         EnsembleUnsaved() const
    {
        return ensemble_baseline_.len > 0 && !(EnsembleSnapshot() == ensemble_baseline_);
    }
    EditSnapshot ensemble_baseline_ = {};
    EditSnapshot ensemble_parked_baseline_ = {}; // see SelectEnsembleEngine()
    EditSnapshot dexed_baseline_    = {};
    EditSnapshot dexed_parked_baseline_ = {}; // see SelectDexedEngine()
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
