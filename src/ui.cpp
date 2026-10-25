#include "ui.h"
#include "performance_store.h"
#include "audio_engine.h"
#include "granular_engine.h"
#include "dexed_synth.h"
#include "sample_synth.h"
#include "msfa/fm_core.h"
#include <cstdio>
#include <cstring>
#include <cctype>
#include <math.h>

using namespace daisy;

namespace
{
inline float Clampf(float v, float lo, float hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

// How long "Nothing to undo" squats on Layer:Status's waveform before an
// idle user gets it back -- see undo_status_set_ms_'s own comment in ui.h.
constexpr uint32_t kUndoStatusTimeoutMs = 1500;

// Set just before a blocking PerformanceStore::Save()/Load() call so the
// static Ui::OnSaveLoadProgress() callback (a plain C function pointer,
// so it can't carry a `this`) has something to draw the progress bar to.
// Safe as a single global: there is exactly one Ui instance and Save()/
// Load() are only ever called synchronously from the main loop.
OneBitGraphicsDisplay* g_progress_disp = nullptr;

// Same "static state read by a plain ProgressFn" shape as
// g_progress_disp above -- set by TriggerSampleSynthPackImport() right
// before each file so Ui::OnSampleSynthImportFileProgress() can
// rescale that one file's own 0..1 sub-progress into its real slice of
// the overall multi-file bar.
float g_progress_file_base01 = 0.f;
float g_progress_file_span01 = 1.f;

const char* StateGlyph(LayerState s)
{
    switch(s)
    {
        case LayerState::Empty: return "-";
        case LayerState::ArmedCountIn: return "C";
        case LayerState::Recording: return "R";
        case LayerState::Playing: return "P";
        case LayerState::Paused: return "||";
        case LayerState::Overdubbing: return "O";
        case LayerState::ArmedOverdubCountIn: return "c";
    }
    return "?";
}

const char* FilterModeName(FilterMode m)
{
    switch(m)
    {
        case FilterMode::Off: return "Off";
        case FilterMode::LowPass: return "LowPass";
        case FilterMode::HighPass: return "HighPass";
        case FilterMode::BandPass: return "BandPass";
        default: return "?";
    }
}

const char* EffectName(LayerEffect e)
{
    switch(e)
    {
        case LayerEffect::Off: return "Off";
        case LayerEffect::Drive: return "Drive";
        case LayerEffect::Bitcrush: return "Bitcrush";
        case LayerEffect::Chorus: return "Chorus";
        case LayerEffect::Tremolo: return "Tremolo";
        case LayerEffect::Phaser: return "Phaser";
        case LayerEffect::AutoWah: return "AutoWah";
        case LayerEffect::Flanger: return "Flanger";
        default: return "?";
    }
}

// Knob 1 / Knob 2 labels for the Effect page -- matches the param
// mapping in LooperLayer::ProcessEffectsChain(). Every mode uses both
// knobs (Drive's ParamB is an output trim, not a core Overdrive param
// -- see ProcessEffectsChain()'s own comment).
const char* EffectParamALabel(LayerEffect e)
{
    switch(e)
    {
        case LayerEffect::Drive: return "Drive";
        case LayerEffect::Bitcrush: return "Crush";
        case LayerEffect::Chorus: return "Depth";
        case LayerEffect::Tremolo: return "Depth";
        case LayerEffect::Phaser: return "Depth";
        case LayerEffect::AutoWah: return "Wah";
        case LayerEffect::Flanger: return "Depth";
        default: return "-";
    }
}
const char* GranularDirectionName(GranularEngine::Direction d)
{
    switch(d)
    {
        case GranularEngine::Direction::Forward: return "Forward";
        case GranularEngine::Direction::Reverse: return "Reverse";
        case GranularEngine::Direction::Random: return "Random";
        default: return "?";
    }
}

const char* EffectParamBLabel(LayerEffect e)
{
    switch(e)
    {
        case LayerEffect::Drive: return "Trim";
        case LayerEffect::Bitcrush: return "Rate";
        case LayerEffect::Chorus: return "Rate";
        case LayerEffect::Tremolo: return "Rate";
        case LayerEffect::Phaser: return "Rate";
        case LayerEffect::AutoWah: return "Level";
        case LayerEffect::Flanger: return "Rate";
        default: return "-";
    }
}
} // namespace

void Ui::Init(daisy::DaisyPod*              pod,
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
              size_t*                       undo_backup_len)
{
    pod_        = pod;
    disp_       = display;
    tempo_      = tempo;
    layers_     = layers;
    num_layers_ = num_layers;
    granular_           = granular;
    granular_scope_buf_      = granular_scope_buf;
    granular_scope_capacity_ = granular_scope_capacity;
    granular_capture_buf_l_     = granular_capture_buf_l;
    granular_capture_buf_r_     = granular_capture_buf_r;
    granular_capture_capacity_  = granular_capture_capacity;
    granular_capturing_         = granular_capturing;
    granular_capture_write_pos_ = granular_capture_write_pos;
    master_scope_buf_      = master_scope_buf;
    master_scope_capacity_ = master_scope_capacity;
    dexed_                 = dexed;
    dexed_scope_buf_        = dexed_scope_buf;
    dexed_scope_capacity_   = dexed_scope_capacity;
    ensemble_               = ensemble;
    ensemble_scope_buf_      = ensemble_scope_buf;
    ensemble_scope_capacity_ = ensemble_scope_capacity;
    sample_synth_                = sample_synth;
    sample_synth_scope_buf_      = sample_synth_scope_buf;
    sample_synth_scope_capacity_ = sample_synth_scope_capacity;
    sample_synth_arena_l_        = sample_synth_arena_l;
    sample_synth_arena_r_        = sample_synth_arena_r;
    sample_synth_arena_capacity_ = sample_synth_arena_capacity;
    sample_synth_capture_buf_l_     = sample_synth_capture_buf_l;
    sample_synth_capture_buf_r_     = sample_synth_capture_buf_r;
    sample_synth_capture_capacity_  = sample_synth_capture_capacity;
    sample_synth_capturing_         = sample_synth_capturing;
    sample_synth_capture_write_pos_ = sample_synth_capture_write_pos;
    undo_backup_l_          = undo_backup_l;
    undo_backup_r_          = undo_backup_r;
    undo_owner_layer_       = undo_owner_layer;
    undo_backup_len_        = undo_backup_len;

    // Same curve ApplyKnobs() uses for knob1 on Home, applied once here
    // so master_volume_ actually matches master_volume01_'s starting
    // value instead of carrying its own separate hardcoded default.
    master_volume_ = powf(master_volume01_, 2.5f) * 1.43f;
    if(master_volume_ < 0.f)
        master_volume_ = 0.f;
    // Same reasoning as master_volume_ just above, for Screen::Mixer's
    // own Bypass channel volume.
    SetBypassMixVolume01(bypass_mix_volume01_);

    // screen_ and last_knob_context_ both default to Home, so the
    // context-change check in ApplyKnobs() that normally calls this
    // never fires for the very first tick after power-on -- without
    // this, Home's pickup targets would sit at their raw 0-default
    // instead of the real starting values above, meaning the knobs
    // would need sweeping down to 0% to regain control rather than to
    // wherever Volume/Metro actually start.
    SyncPickupTargets(KnobContext::Home);
}

void Ui::ApplyStartupDefaults()
{
    float      bpm, master_vol01, cutoff01, res01, reverb_sz01, byp_send01, metro_vol01;
    float      rev_predelay01, rev_prefilter01, rev_diff1_01, rev_diff2_01, rev_decdiff01,
        rev_damping01;
    int        bars, dexed_ch, granular_ch, ensemble_ch, sample_synth_ch;
    bool       byp, metro_on;
    FilterMode mode;
    if(!PerformanceStore::LoadPrefs(&bpm, &bars, &master_vol01, &byp, &mode, &cutoff01,
                                      &res01, &reverb_sz01, &byp_send01, &metro_on,
                                      &metro_vol01, &rev_predelay01, &rev_prefilter01,
                                      &rev_diff1_01, &rev_diff2_01, &rev_decdiff01,
                                      &rev_damping01, &dexed_ch, &granular_ch, &ensemble_ch,
                                      &sample_synth_ch))
        return; // nothing saved yet -- Init()'s hardcoded defaults already stand

    // SetBpm()/SetBars() are no-ops while locked, but nothing can be
    // locked this early (no layer has recorded anything yet).
    tempo_->SetBpm(bpm);
    tempo_->SetBars(bars);
    tempo_->SetMetronomeEnabled(metro_on);
    tempo_->SetMetronomeVolume01(metro_vol01);

    master_volume01_ = master_vol01;
    master_volume_   = powf(Clampf(master_volume01_, 0.f, 1.f), 2.5f) * 1.43f;
    if(master_volume_ < 0.f)
        master_volume_ = 0.f;

    bypass_                  = byp;
    master_filter_mode_      = mode;
    master_filter_cutoff01_  = cutoff01;
    master_filter_res01_     = res01;
    reverb_size01_           = reverb_sz01;
    bypass_reverb_send01_    = byp_send01;
    reverb_predelay01_          = rev_predelay01;
    reverb_prefilter01_         = rev_prefilter01;
    reverb_input_diffusion1_01_ = rev_diff1_01;
    reverb_input_diffusion2_01_ = rev_diff2_01;
    reverb_decay_diffusion01_   = rev_decdiff01;
    reverb_damping01_           = rev_damping01;
    dexed_midi_channel_         = dexed_ch;
    granular_midi_channel_      = granular_ch;
    ensemble_midi_channel_      = ensemble_ch;
    sample_synth_midi_channel_  = sample_synth_ch;

    // Init() already called this once with the pre-defaults values (see
    // its own comment above) -- re-seed now that the real starting
    // values are in place, same reasoning, same fix.
    SyncPickupTargets(KnobContext::Home);
}

void Ui::SetBypassMixVolume01(float v01)
{
    bypass_mix_volume01_ = Clampf(v01, 0.f, 1.f);
    bypass_mix_volume_   = powf(bypass_mix_volume01_, 2.5f) * 1.4f;
    if(bypass_mix_volume_ < 0.f)
        bypass_mix_volume_ = 0.f;
}

void Ui::Update(const UiControlEvents& events)
{
    HandleEncoder(events);
    HandleButton1(events);
    HandleButton2(events);
    ApplyKnobs();

    // Grains' Rhythm Speed has a "Sync" option that locks to the
    // project's live tempo -- the engine has no tempo concept of its
    // own otherwise, so Ui (which already owns the TempoClock) pushes
    // the current BPM in once per tick, cheap enough to do unconditionally
    // rather than only when Speed is actually set to Sync.
    if(granular_ && tempo_)
        granular_->SetExternalBpm(tempo_->GetBpm());

    // Once any layer holds a recording, lock the tempo so BPM/Bars can't
    // be changed out from under it; unlock once every layer is empty
    // again so a fresh song can pick a new tempo.
    bool any_content = false;
    for(int i = 0; i < num_layers_; i++)
        if(layers_[i].HasContent())
            any_content = true;
    if(any_content)
        tempo_->Lock();
    else
        tempo_->Unlock();

    UpdateLeds();

    // The OLED redraw is a blocking I2C transfer of ~1KB -- a few
    // milliseconds. Throttled to ~30Hz here; Update() is expected to be
    // called at roughly 500Hz-1kHz from main()'s loop (see main.cpp), so
    // redrawing every 20th call keeps the screen responsive without
    // hogging the I2C bus or the main loop.
    draw_counter_++;
    if(draw_counter_ >= 20)
    {
        draw_counter_ = 0;
        Draw();
    }
}

// --- Input handling ------------------------------------------------------

void Ui::HandleEncoder(const UiControlEvents& events)
{
    int32_t inc = events.encoder_delta;
    if(inc != 0)
    {
        if(speed_transport_mode_ == SpeedTransportMode::Scrub && screen_ == Screen::Global && global_page_ == GlobalPage::Speed)
        {
            ScrubBy(inc);
        }
        else if(speed_transport_mode_ == SpeedTransportMode::Freeze && screen_ == Screen::Global && global_page_ == GlobalPage::Speed)
        {
            NudgeFreezeBy(inc);
        }
        else if(screen_ == Screen::Home)
        {
            cursor_layer_ = ((cursor_layer_ + inc) % num_layers_ + num_layers_) % num_layers_;
        }
        else if(screen_ == Screen::Layer)
        {
            int n = (int)LayerPage::kCount;
            int p = (((int)layer_page_ + inc) % n + n) % n;
            layer_page_ = (LayerPage)p;
        }
        else if(screen_ == Screen::Global)
        {
            int n = (int)GlobalPage::kCount;
            int p = (((int)global_page_ + inc) % n + n) % n;
            GlobalPage new_page = (GlobalPage)p;
            if(new_page == GlobalPage::File && global_page_ != GlobalPage::File)
                file_slots_dirty_ = true; // re-scan the card on entry
            if(new_page == GlobalPage::Export && global_page_ != GlobalPage::Export)
                export_status_[0] = '\0'; // clear any stale result on entry
            if(new_page == GlobalPage::SdMgmt && global_page_ != GlobalPage::SdMgmt)
                sd_mgmt_in_folder_ = false; // always start back at folder-select on entry
            global_page_    = new_page;
            save_load_mode_ = SaveLoadMode::Idle; // leaving/entering any page resets this
        }
        else if(screen_ == Screen::Granular)
        {
            int n = (int)GranularParamPage::kCount;
            int p = (((int)granular_param_page_ + inc) % n + n) % n;
            GranularParamPage new_granular_page = (GranularParamPage)p;
            if(new_granular_page == GranularParamPage::Preset
               && granular_param_page_ != GranularParamPage::Preset)
                granular_preset_slots_dirty_ = true; // re-scan the card on entry
            granular_param_page_ = new_granular_page;
            save_load_mode_      = SaveLoadMode::Idle; // same reset as Global:File above
        }
        else if(screen_ == Screen::Dexed)
        {
            int n = (int)DexedParamPage::kCount;
            int p = (((int)dexed_param_page_ + inc) % n + n) % n;
            DexedParamPage new_dexed_page = (DexedParamPage)p;
            if(new_dexed_page == DexedParamPage::Preset
               && dexed_param_page_ != DexedParamPage::Preset)
            {
                dexed_preset_slots_dirty_ = true; // re-scan the card on entry
                dexed_preset_folder_open_ = false; // always start at the folder list
            }
            dexed_param_page_ = new_dexed_page;
            save_load_mode_   = SaveLoadMode::Idle; // same reset as Global:File above
        }
        else if(screen_ == Screen::DexedOperator)
        {
            int n = (int)DexedOpParamPage::kCount;
            int p = (((int)dexed_op_page_ + inc) % n + n) % n;
            dexed_op_page_ = (DexedOpParamPage)p;
        }
        else if(screen_ == Screen::Ensemble)
        {
            int n = (int)EnsembleParamPage::kCount;
            int p = (((int)ensemble_param_page_ + inc) % n + n) % n;
            EnsembleParamPage new_ensemble_page = (EnsembleParamPage)p;
            if(new_ensemble_page == EnsembleParamPage::Preset
               && ensemble_param_page_ != EnsembleParamPage::Preset)
                ensemble_preset_slots_dirty_ = true; // re-scan the card on entry
            ensemble_param_page_ = new_ensemble_page;
            save_load_mode_      = SaveLoadMode::Idle; // same reset as Global:File above
        }
        else if(screen_ == Screen::SampleSynth)
        {
            int n = (int)SampleSynthParamPage::kCount;
            int p = (((int)sample_synth_param_page_ + inc) % n + n) % n;
            // Skip pages that don't belong to the current face -- Setup
            // is Drum-only (Break Divide has no meaning for a single
            // melodic Sound), Capture/Trim/Tune are Voice-only (SMPDR
            // has no capture feature at all, only Import). Steps by
            // +-1 regardless of the real encoder delta's own magnitude
            // (unlike the jump above) so this always visits every page
            // on its way around rather than potentially cycling through
            // only a fixed subset of residues forever if the delta and
            // page count ever shared a common factor.
            int step = inc > 0 ? 1 : -1;
            while(!IsSampleSynthPageValidForMode((SampleSynthParamPage)p, sample_synth_mode_))
                p = ((p + step) % n + n) % n;
            SampleSynthParamPage new_page = (SampleSynthParamPage)p;
            if(new_page == SampleSynthParamPage::Preset
               && sample_synth_param_page_ != SampleSynthParamPage::Preset)
            {
                MarkSampleSynthListsDirty(); // re-scan the card on entry
            }
            else if(new_page == SampleSynthParamPage::User
                    && sample_synth_param_page_ != SampleSynthParamPage::User)
            {
                // Re-scan whichever face's own User-preset list on
                // entry -- same shape as Preset's own Kits/Sounds dirty
                // flags just above.
                if(sample_synth_mode_ == SampleSynthMode::Voice)
                    sample_synth_voice_user_dirty_ = true;
                else
                    sample_synth_drum_user_dirty_ = true;
            }
            else if(new_page == SampleSynthParamPage::Capture
                    && sample_synth_param_page_ != SampleSynthParamPage::Capture
                    && sample_synth_param_page_ != SampleSynthParamPage::Trim
                    && sample_synth_param_page_ != SampleSynthParamPage::Tune)
            {
                // Only reset the status line on actually landing on
                // Capture itself (not Trim/Tune, and not just passing
                // through on the way to some other page entirely --
                // see OnEnterSampleSynthCaptureGroup()'s own comment
                // for why the ACTUAL slot wipe moved out of here).
                OnEnterSampleSynthCaptureGroup();
            }
            sample_synth_param_page_ = new_page;
            save_load_mode_          = SaveLoadMode::Idle; // same reset as Global:File above
        }
        else if(screen_ == Screen::Mixer)
        {
            int n            = kNumMixerPositions;
            int new_position = ((mixer_position_ + inc) % n + n) % n;
            if(new_position != mixer_position_)
            {
                mixer_position_ = new_position;
                // Same reset/reseed CurrentKnobContext()'s own change
                // would normally trigger automatically in ApplyKnobs()
                // (see MixerVolPan/MixerReverb/MixerMaster's own comment
                // for why this needs doing explicitly here instead) --
                // without this, turning past a channel whose knob context
                // enum doesn't itself change (any of the 7 non-Master
                // channels) would leave the knobs pointing at the OLD
                // channel's cached pickup values instead of the new one's.
                KnobContext ctx        = CurrentKnobContext();
                size_t      ctx_index  = (size_t)ctx;
                k1_pickup_engaged_[ctx_index] = false;
                k2_pickup_engaged_[ctx_index] = false;
                SyncPickupTargets(ctx);
                last_knob_context_ = ctx;
            }
        }
    }

    if(pod_->encoder.Pressed() && pod_->encoder.TimeHeldMs() > 600.f && !encoder_long_fired_)
    {
        encoder_long_fired_ = true;
        if(screen_ == Screen::Home)
        {
            screen_      = Screen::Global;
            global_page_ = GlobalPage::Tempo;
            // This hand-wired SD socket has no card-detect pin, so the
            // firmware otherwise never notices a card was swapped out
            // (e.g. pulled to move a WAV export to another device) and
            // put back -- re-mount here so Global:File/Export reflect
            // whatever card is actually in the slot right now, rather
            // than a stale mount from boot.
            PerformanceStore::Remount();
        }
        else
        {
            screen_ = Screen::Home;
        }
    }

    // "Click" (drill into the cursor layer) only fires on release, and
    // only if this press didn't already turn out to be a long-press --
    // otherwise a long-press from Home would drill into Layer on the
    // initial press-down before the 600ms check ever saw screen_ as
    // Home, turning every long-press into an immediate Layer->Home
    // bounce instead of reaching Global.
    if(events.encoder_click_fell)
    {
        if(!encoder_long_fired_ && screen_ == Screen::Home)
        {
            screen_     = Screen::Layer;
            layer_page_ = LayerPage::Status;
        }
        else if(!encoder_long_fired_ && screen_ == Screen::Global
                && global_page_ == GlobalPage::Granular)
        {
            // Global:Granular is an entry point into Screen::Granular,
            // same convention Screen::Mixer's own Global page uses.
            screen_ = Screen::Granular;
        }
        else if(!encoder_long_fired_ && screen_ == Screen::Global
                && global_page_ == GlobalPage::Looper)
        {
            // No screen to drill into (Button1 tap is this page's own
            // toggle) -- explicitly excluded from TogglePauseAll() below
            // so this page's click can't accidentally trigger a second,
            // different "stop the loop" mechanism.
        }
        else if(!encoder_long_fired_ && screen_ == Screen::Global
                && global_page_ == GlobalPage::Mixer)
        {
            // Global:Mixer is an entry point into Screen::Mixer, same
            // convention Global:Pad/Global:Granular's own click uses.
            screen_ = Screen::Mixer;
        }
        else if(!encoder_long_fired_ && screen_ == Screen::Global
                && global_page_ == GlobalPage::Dexed)
        {
            // Global:Dexed is an entry point into Screen::Dexed, same
            // convention Global:Granular's own click uses.
            screen_ = Screen::Dexed;
        }
        else if(!encoder_long_fired_ && screen_ == Screen::Global
                && global_page_ == GlobalPage::Ensemble)
        {
            // Global:Ensemble is an entry point into Screen::Ensemble,
            // same convention Global:Granular/Global:Dexed's own click
            // uses.
            screen_ = Screen::Ensemble;
        }
        else if(!encoder_long_fired_ && screen_ == Screen::Global
                && (global_page_ == GlobalPage::SampleSynthVoice
                     || global_page_ == GlobalPage::SampleSynthDrum))
        {
            // Global:SMPVO/Global:SMPDR are both entry points into the
            // same Screen::SampleSynth, same convention Global:Granular/
            // Dexed/Ensemble's own click uses -- which one just sets
            // which "face" (SwitchSampleSynthMode()'s own doc comment).
            // Only actually switch (and pay its reload cost) if this
            // click is genuinely changing which face is showing --
            // re-clicking the one you're already on just re-enters the
            // screen cheaply, same as every other engine's own entry
            // click already does.
            SampleSynthMode target = global_page_ == GlobalPage::SampleSynthVoice
                                          ? SampleSynthMode::Voice
                                          : SampleSynthMode::Drum;
            if(target != sample_synth_mode_)
                SwitchSampleSynthMode(target);
            // Drilling in also enables that face (and, by the same
            // interlock the Button1 toggle uses, disables the other) --
            // a real reported bug otherwise: drilling into one while
            // the OTHER was still the enabled one, after explicitly
            // disabling it first, played nothing at all, since neither
            // flag ended up true. Being on this screen IS "this one's
            // live" -- same mental model as every other engine, where
            // there's no separate "enabled" state to forget about.
            if(target == SampleSynthMode::Voice)
            {
                sample_synth_voice_enabled_ = true;
                sample_synth_drum_enabled_  = false;
            }
            else
            {
                sample_synth_drum_enabled_  = true;
                sample_synth_voice_enabled_ = false;
            }
            if(sample_synth_)
                sample_synth_->AllNotesOff();
            screen_ = Screen::SampleSynth;
        }
        else if(!encoder_long_fired_ && screen_ == Screen::Global)
        {
            TogglePauseAll();
        }
        else if(!encoder_long_fired_ && screen_ == Screen::Dexed
                && dexed_param_page_ == DexedParamPage::Advanced)
        {
            // Dexed's own Advanced page is an entry point into
            // Screen::DexedOperator, same convention Global:Granular/
            // Global:Dexed's own click uses to enter their screens.
            screen_ = Screen::DexedOperator;
        }
        else if(!encoder_long_fired_ && screen_ == Screen::DexedOperator)
        {
            // Short click backs out to Screen::Dexed (landing back on
            // the Advanced page); long-press above still goes all the
            // way to Home, same as every other screen.
            screen_ = Screen::Dexed;
        }
        else if(!encoder_long_fired_
                && (screen_ == Screen::Granular || screen_ == Screen::Dexed
                    || screen_ == Screen::Ensemble || screen_ == Screen::SampleSynth))
        {
            // Same mute-all-loop-layers click as every Global page's own
            // (TogglePauseAll()) -- every other Granular/Dexed/Ensemble/
            // SampleSynth page has no click-to-drill-in of their own
            // (that's the encoder's job from Global instead, or Dexed's
            // own Advanced page above -- Sounds' own category level (see
            // sample_synth_sound_category_'s own comment in ui.h) drills
            // via Button2 instead, same button its "Open"/"Load" footer
            // label already names -- see OnButton2Short()), so there's
            // nothing else useful for the encoder's own click to do here
            // either.
            TogglePauseAll();
        }
        // Screen::Mixer: rotate already picks the stop (see
        // HandleEncoder()'s own rotate handling above, including the
        // Scope stop) -- click is unused here, same as every other
        // Granular-style screen.
        encoder_long_fired_ = false;
    }

    // Scrub/Freeze are Global:Speed-only, always-transient modes -- clear
    // them unconditionally the instant we're not there any more (long-press
    // to Home above is the only current exit path, but this isn't tied to
    // that one call site specifically so it can't be left silently stuck
    // on by any future navigation change). Leaving Freeze specifically
    // must also un-freeze every layer, or playback would stay stuck on
    // the tiny drone window with no way back in from this screen.
    if(!(screen_ == Screen::Global && global_page_ == GlobalPage::Speed))
    {
        if(speed_transport_mode_ == SpeedTransportMode::Freeze)
            for(int i = 0; i < num_layers_; i++)
                layers_[i].SetFreezeActive(false);
        speed_transport_mode_ = SpeedTransportMode::Normal;
    }
}

void Ui::HandleButton1(const UiControlEvents& events)
{
    Switch& b = pod_->button1;
    if(b.Pressed() && b.TimeHeldMs() > 400.f && !button1_long_fired_)
    {
        button1_long_fired_ = true;
        OnButton1Long();
    }
    if(events.btn1_released)
    {
        // Fallback for when the main loop never got a chance to observe
        // "still held past threshold" before the release happened -- see
        // UiControlEvents::btn1_held_ms/btn2_held_ms's own comment. Every
        // long-press site in this function and HandleButton2() below
        // needs this same fallback, not just this one.
        if(!button1_long_fired_ && events.btn1_held_ms > 400.f)
        {
            OnButton1Long();
            button1_long_fired_ = true;
        }
        if(!button1_long_fired_)
            OnButton1Short();
        else
            OnButton1Release();
        button1_long_fired_ = false;
    }
}

void Ui::HandleButton2(const UiControlEvents& events)
{
    Switch& b = pod_->button2;
    // Button2's only "long" behaviours are the hold-to-clear confirm on
    // the layer Status page and the hold-to-load confirm on Global:File;
    // everywhere else it's a plain short-press action.
    if(screen_ == Screen::Layer && layer_page_ == LayerPage::Status)
    {
        if(b.Pressed() && b.TimeHeldMs() > 800.f && !button2_long_fired_)
        {
            button2_long_fired_ = true;
            Cur().Clear();
        }
        if(events.btn2_released)
        {
            // Fallback for a press/hold/release cycle that completed
            // between main-loop iterations -- see HandleButton1()'s
            // matching comment and main.cpp's g_btn2_held_ms.
            if(!button2_long_fired_ && events.btn2_held_ms > 800.f)
                Cur().Clear();
            else if(!button2_long_fired_)
                TriggerUndoOverdub(); // a genuine short tap -- see its own comment
            button2_long_fired_ = false;
        }
    }
    else if(screen_ == Screen::Global && global_page_ == GlobalPage::SdMgmt
            && sd_mgmt_in_folder_)
    {
        // Inside a bank box's bank list there's nothing to delete yet --
        // a short tap opens the highlighted bank instead (same "Open"
        // tap as the Sampler's own Preset browser).
        bool at_bank_list = SdMgmtIsBankFolder() && sd_mgmt_bank_ < 0;
        // Longer hold than every other confirm gesture here (see
        // kSdMgmtDeleteHoldMs's own comment) -- delete has no undo.
        if(!at_bank_list && b.Pressed() && b.TimeHeldMs() > kSdMgmtDeleteHoldMs
           && !button2_long_fired_)
        {
            button2_long_fired_ = true;
            TriggerSdMgmtDelete();
        }
        if(events.btn2_released)
        {
            if(at_bank_list)
            {
                if(sd_mgmt_cursor_ < sd_mgmt_bank_count_)
                {
                    sd_mgmt_bank_        = sd_mgmt_cursor_;
                    sd_mgmt_cursor_      = 0;
                    sd_mgmt_items_dirty_ = true;
                    sd_mgmt_status_[0]   = '\0';
                }
            }
            else if(!button2_long_fired_ && events.btn2_held_ms > kSdMgmtDeleteHoldMs)
                TriggerSdMgmtDelete();
            button2_long_fired_ = false;
        }
    }
    else if(screen_ == Screen::Global && global_page_ == GlobalPage::File)
    {
        // Button2 confirms whichever of the two revealed states is
        // active -- Save (ChoosingSave) or Load (BrowsingLoad) -- so
        // Button1's own tap is free to always mean "Back" once inside
        // either one (see OnButton1Short()), same as SD MGMT's own
        // Back/Hold=Duplicate split.
        bool can_confirm_save = save_load_mode_ == SaveLoadMode::ChoosingSave;
        // BrowsingLoad only has something to confirm once either "New" is
        // picked at the top-level chooser or the numbered list has been
        // drilled into (see ApplyKnobs()'s own BrowsingLoad handling and
        // load_new_selected_/load_browsing_files_'s own comment) -- at
        // the chooser with "Files" picked there's no specific target yet,
        // so a hold there does nothing.
        bool can_confirm_load = save_load_mode_ == SaveLoadMode::BrowsingLoad
                                 && (load_browsing_files_ || load_new_selected_);
        bool can_confirm      = can_confirm_save || can_confirm_load;
        if(b.Pressed() && b.TimeHeldMs() > 800.f && !button2_long_fired_ && can_confirm)
        {
            button2_long_fired_ = true;
            if(can_confirm_save)
                TriggerSave(save_as_new_);
            else
                TriggerLoad();
            save_load_mode_ = SaveLoadMode::Idle;
        }
        if(events.btn2_released)
        {
            if(!button2_long_fired_ && events.btn2_held_ms > 800.f && can_confirm)
            {
                if(can_confirm_save)
                    TriggerSave(save_as_new_);
                else
                    TriggerLoad();
                save_load_mode_ = SaveLoadMode::Idle;
            }
            else if(!button2_long_fired_ && save_load_mode_ == SaveLoadMode::Idle)
            {
                // Short tap from Idle only: reveal the file list to
                // browse (Button2's own label becomes "Hold=Load" to
                // commit). While already in ChoosingSave/BrowsingLoad, a
                // short tap here does nothing new -- only the hold above
                // commits, and Button1's tap is "Back".
                if(!PerformanceStore::IsCardPresent())
                    PerformanceStore::Remount();
                else
                {
                    save_load_mode_     = SaveLoadMode::BrowsingLoad;
                    load_new_selected_  = false;
                    load_browsing_files_ = false;
                }
            }
            button2_long_fired_ = false;
        }
    }
    else if(screen_ == Screen::Granular && granular_param_page_ == GranularParamPage::Capture)
    {
        if(granular_capture_source_ >= 0)
        {
            // Instant copy -- same hold-to-confirm-then-fire-once
            // gesture as every other destructive/overwriting action in
            // this project (Load/New/Clear).
            if(b.Pressed() && b.TimeHeldMs() > 800.f && !button2_long_fired_)
            {
                button2_long_fired_ = true;
                TriggerGranularCaptureFromLayer();
            }
            if(events.btn2_released)
            {
                if(!button2_long_fired_ && events.btn2_held_ms > 800.f)
                    TriggerGranularCaptureFromLayer();
                button2_long_fired_ = false;
            }
        }
        else if(granular_capture_source_ == -2)
        {
            // Import -- same hold-to-confirm-then-fire-once gesture as
            // From Layer above (a real SD read + decode, not instant, but
            // still a single fire-once action, not a live-recording
            // gesture like Direct Record below).
            if(b.Pressed() && b.TimeHeldMs() > 800.f && !button2_long_fired_)
            {
                button2_long_fired_ = true;
                TriggerGranularImport();
            }
            if(events.btn2_released)
            {
                if(!button2_long_fired_ && events.btn2_held_ms > 800.f)
                    TriggerGranularImport();
                button2_long_fired_ = false;
            }
        }
        else if(granular_capturing_ && granular_capture_write_pos_)
        {
            // Direct Record: holding Button2 IS recording (live input
            // written in from AudioCallback() while *granular_capturing_
            // is true) -- release stops it, and so does the ISR itself
            // once the 5s buffer fills while still held (granular_was_
            // capturing_ catches both: it only clears once *granular_
            // capturing_ has actually gone false).
            bool pressed_now = b.Pressed();
            if(pressed_now && !granular_was_capturing_)
            {
                *granular_capture_write_pos_ = 0;
                *granular_capturing_         = true;
                granular_was_capturing_      = true;
            }
            else if(granular_was_capturing_ && (!pressed_now || !*granular_capturing_))
            {
                *granular_capturing_ = false;
                if(granular_)
                    granular_->SetSource(granular_capture_buf_l_, granular_capture_buf_r_,
                                          *granular_capture_write_pos_);
                OnNewGranularCapture(*granular_capture_write_pos_);
                granular_capture_status_[0] = '\0'; // no From-Layer status to show any more
                granular_was_capturing_     = false;
            }
        }
    }
    else if(screen_ == Screen::Granular && granular_param_page_ == GranularParamPage::Preset)
    {
        // Button2 confirms whichever of Save/Load is revealed -- see
        // Global:File's own Button2 handling for the shared SaveLoadMode
        // this mirrors.
        bool can_confirm_save = save_load_mode_ == SaveLoadMode::ChoosingSave;
        // BrowsingLoad only has something to confirm once either "New" is
        // picked at the top-level chooser or the numbered list has been
        // drilled into (see Global:File's own can_confirm_load comment).
        bool can_confirm_load = save_load_mode_ == SaveLoadMode::BrowsingLoad
                                 && (load_browsing_files_ || load_new_selected_);
        bool can_confirm      = can_confirm_save || can_confirm_load;
        if(b.Pressed() && b.TimeHeldMs() > 800.f && !button2_long_fired_ && can_confirm)
        {
            button2_long_fired_ = true;
            if(can_confirm_save)
                TriggerSaveGranularPreset(save_as_new_);
            else
                TriggerLoadGranularPreset();
            save_load_mode_ = SaveLoadMode::Idle;
        }
        if(events.btn2_released)
        {
            if(!button2_long_fired_ && events.btn2_held_ms > 800.f && can_confirm)
            {
                if(can_confirm_save)
                    TriggerSaveGranularPreset(save_as_new_);
                else
                    TriggerLoadGranularPreset();
                save_load_mode_ = SaveLoadMode::Idle;
            }
            else if(!button2_long_fired_ && save_load_mode_ == SaveLoadMode::Idle)
            {
                // Short tap from Idle only: reveal the preset list to
                // browse.
                if(!PerformanceStore::IsCardPresent())
                    PerformanceStore::Remount();
                else
                {
                    save_load_mode_      = SaveLoadMode::BrowsingLoad;
                    load_new_selected_   = false;
                    load_browsing_files_ = false;
                }
            }
            button2_long_fired_ = false;
        }
    }
    else if(screen_ == Screen::Dexed && dexed_param_page_ == DexedParamPage::Preset)
    {
        // Button2 confirms whichever of Save/Load is revealed -- see
        // Global:File's own Button2 handling for the shared SaveLoadMode
        // this mirrors.
        bool can_confirm_save = save_load_mode_ == SaveLoadMode::ChoosingSave;
        // Nothing concrete is highlighted while just browsing a group or
        // folder list (names aren't presets) -- confirm only once "New"
        // is picked at the top-level chooser, a folder has actually been
        // opened onto a real preset (Roms/Dexed/Imports), the User group
        // is open (it has no folder level of its own, see
        // DexedFilesGroup's own comment), or a real .syx file is
        // highlighted inside the Import list.
        bool dexed_files_ready = load_browsing_files_ && dexed_files_group_open_
                                   && (dexed_preset_folder_open_
                                       || dexed_files_group_ == DexedFilesGroup::User);
        bool can_confirm_load = save_load_mode_ == SaveLoadMode::BrowsingLoad
                                 && (load_new_selected_ || dexed_files_ready
                                     || (dexed_import_browsing_ && dexed_import_file_count_ > 0));
        bool can_confirm      = can_confirm_save || can_confirm_load;
        if(b.Pressed() && b.TimeHeldMs() > 800.f && !button2_long_fired_ && can_confirm)
        {
            button2_long_fired_ = true;
            if(can_confirm_save)
                TriggerSaveDexedPreset(save_as_new_);
            else
                TriggerLoadDexedPreset();
            save_load_mode_ = SaveLoadMode::Idle;
        }
        if(events.btn2_released)
        {
            if(!button2_long_fired_ && events.btn2_held_ms > 800.f && can_confirm)
            {
                if(can_confirm_save)
                    TriggerSaveDexedPreset(save_as_new_);
                else
                    TriggerLoadDexedPreset();
                save_load_mode_ = SaveLoadMode::Idle;
            }
            else if(!button2_long_fired_ && save_load_mode_ == SaveLoadMode::BrowsingLoad
                     && dexed_files_ready)
            {
                // Short tap while a preset is highlighted (an open
                // folder in Roms/Dexed/Imports, or anywhere in the flat
                // User list) -- preview it immediately (apply to the
                // live engine) WITHOUT leaving the browser, so scrolling
                // K1 and tapping B2 auditions one preset after another
                // without re-entering the whole Save/Load flow each
                // time. Button2's hold gesture above still does the same
                // load AND exits back to Idle, for once you've settled
                // on one.
                TriggerLoadDexedPreset();
            }
            else if(!button2_long_fired_ && save_load_mode_ == SaveLoadMode::BrowsingLoad
                     && load_browsing_files_ && !dexed_files_ready)
            {
                // "Open" -- Button2's own role specifically at the group
                // chooser and folder-list levels (Button1 is "Back"
                // there instead, see OnButton1Short()) -- drills one
                // level deeper, the same target state Button1 used to
                // set directly before this Back/Open split.
                if(!dexed_files_group_open_)
                {
                    dexed_files_group_open_     = true;
                    dexed_preset_folder_open_   = false;
                    dexed_preset_folder_cursor_ = 0;
                    dexed_preset_cursor_        = 0;
                    dexed_preset_slots_dirty_   = true;
                }
                else if(dexed_files_group_ != DexedFilesGroup::User
                         && !dexed_preset_folder_open_)
                {
                    dexed_preset_folder_open_ = true;
                    dexed_preset_cursor_      = 0;
                }
            }
            else if(!button2_long_fired_ && save_load_mode_ == SaveLoadMode::Idle)
            {
                // Short tap from Idle only: reveal the preset list to
                // browse (Button2's own label becomes "Hold=Load" to
                // commit).
                if(!PerformanceStore::IsCardPresent())
                    PerformanceStore::Remount();
                else
                {
                    save_load_mode_           = SaveLoadMode::BrowsingLoad;
                    load_new_selected_        = false;
                    load_browsing_files_      = false;
                    dexed_files_group_open_   = false; // always start at the group chooser
                    dexed_files_group_        = DexedFilesGroup::Roms;
                    dexed_preset_folder_open_ = false; // always start at the folder list
                    dexed_import_selected_    = false;
                    dexed_import_browsing_    = false;
                }
            }
            button2_long_fired_ = false;
        }
    }
    else if(screen_ == Screen::Ensemble && ensemble_param_page_ == EnsembleParamPage::Preset)
    {
        // Button2 confirms whichever of Save/Load is revealed -- same
        // shared SaveLoadMode shape as Grains'/Dexed's own Preset page.
        bool can_confirm_save = save_load_mode_ == SaveLoadMode::ChoosingSave;
        bool can_confirm_load = save_load_mode_ == SaveLoadMode::BrowsingLoad
                                 && (load_browsing_files_ || load_new_selected_);
        bool can_confirm      = can_confirm_save || can_confirm_load;
        if(b.Pressed() && b.TimeHeldMs() > 800.f && !button2_long_fired_ && can_confirm)
        {
            button2_long_fired_ = true;
            if(can_confirm_save)
                TriggerSaveEnsemblePreset(save_as_new_);
            else
                TriggerLoadEnsemblePreset();
            save_load_mode_ = SaveLoadMode::Idle;
        }
        if(events.btn2_released)
        {
            if(!button2_long_fired_ && events.btn2_held_ms > 800.f && can_confirm)
            {
                if(can_confirm_save)
                    TriggerSaveEnsemblePreset(save_as_new_);
                else
                    TriggerLoadEnsemblePreset();
                save_load_mode_ = SaveLoadMode::Idle;
            }
            else if(!button2_long_fired_ && save_load_mode_ == SaveLoadMode::BrowsingLoad
                     && load_browsing_files_)
            {
                // Short tap while browsing the combined [factory][user]
                // list -- preview it immediately (apply to the live
                // engine) WITHOUT leaving the browser, same "preview ==
                // commit, just without exiting" idiom as
                // TriggerLoadDexedPreset()'s own short-tap behavior.
                TriggerLoadEnsemblePreset();
            }
            else if(!button2_long_fired_ && save_load_mode_ == SaveLoadMode::Idle)
            {
                // Short tap from Idle only: reveal the preset list to
                // browse.
                if(!PerformanceStore::IsCardPresent())
                    PerformanceStore::Remount();
                else
                {
                    save_load_mode_      = SaveLoadMode::BrowsingLoad;
                    load_new_selected_   = false;
                    load_browsing_files_ = false;
                }
            }
            button2_long_fired_ = false;
        }
    }
    else if(screen_ == Screen::SampleSynth && sample_synth_param_page_ == SampleSynthParamPage::Capture)
    {
        // Same 3-way split as GranularParamPage::Capture's own Button2
        // handling -- From Layer/Import are hold-to-confirm-then-fire-
        // once; Direct Record IS the recording for as long as it's held.
        if(sample_synth_capture_source_ >= 0)
        {
            if(b.Pressed() && b.TimeHeldMs() > 800.f && !button2_long_fired_)
            {
                button2_long_fired_ = true;
                TriggerSampleSynthCaptureFromLayer();
            }
            if(events.btn2_released)
            {
                if(!button2_long_fired_ && events.btn2_held_ms > 800.f)
                    TriggerSampleSynthCaptureFromLayer();
                button2_long_fired_ = false;
            }
        }
        else if(sample_synth_capture_source_ == -2)
        {
            if(b.Pressed() && b.TimeHeldMs() > 800.f && !button2_long_fired_)
            {
                button2_long_fired_ = true;
                TriggerSampleSynthImport();
            }
            if(events.btn2_released)
            {
                if(!button2_long_fired_ && events.btn2_held_ms > 800.f)
                    TriggerSampleSynthImport();
                button2_long_fired_ = false;
            }
        }
        else if(sample_synth_capturing_ && sample_synth_capture_write_pos_)
        {
            bool pressed_now = b.Pressed();
            if(pressed_now && !sample_synth_was_capturing_)
            {
                *sample_synth_capture_write_pos_ = 0;
                *sample_synth_capturing_         = true;
                sample_synth_was_capturing_      = true;
            }
            else if(sample_synth_was_capturing_ && (!pressed_now || !*sample_synth_capturing_))
            {
                *sample_synth_capturing_ = false;
                if(sample_synth_)
                {
                    ClearSampleSynthSlotsForFreshCapture();
                    sample_synth_->LoadSlot(0, sample_synth_capture_buf_l_,
                                              sample_synth_capture_buf_r_,
                                              (int32_t)*sample_synth_capture_write_pos_,
                                              /*chromatic=*/true,
                                              sample_synth_tune_root_note_, 0, 127, 0.8f);
                }
                OnNewSampleSynthCapture(*sample_synth_capture_write_pos_);
                sample_synth_capture_status_[0] = '\0'; // no From-Layer/Import status to show any more
                sample_synth_was_capturing_      = false;
            }
        }
    }
    else if(screen_ == Screen::SampleSynth && sample_synth_param_page_ == SampleSynthParamPage::Trim)
    {
        // Hold-to-confirm Save -- same 800ms threshold/button2_long_fired_
        // guard as every other hold-to-confirm action in this project.
        // Unlike Grains (which saves a whole self-contained preset on its
        // own separate Preset page), Trim itself is where a capture
        // becomes a real Sound file -- see TriggerSaveSampleSynthCapture()'s
        // own comment.
        if(b.Pressed() && b.TimeHeldMs() > 800.f && !button2_long_fired_)
        {
            button2_long_fired_ = true;
            TriggerSaveSampleSynthCapture();
        }
        if(events.btn2_released)
        {
            if(!button2_long_fired_ && events.btn2_held_ms > 800.f)
                TriggerSaveSampleSynthCapture();
            button2_long_fired_ = false;
        }
    }
    else if(screen_ == Screen::SampleSynth
            && sample_synth_param_page_ == SampleSynthParamPage::Import)
    {
        // Hold-to-confirm, same 800ms threshold/button2_long_fired_
        // guard as every other hold-to-confirm batch action in this
        // project -- this one can take a few real seconds (one AIFF
        // decode per file), same class of action as Dexed's own SysEx
        // bulk import.
        if(b.Pressed() && b.TimeHeldMs() > 800.f && !button2_long_fired_)
        {
            button2_long_fired_ = true;
            TriggerSampleSynthPackImport();
        }
        if(events.btn2_released)
        {
            if(!button2_long_fired_ && events.btn2_held_ms > 800.f)
                TriggerSampleSynthPackImport();
            button2_long_fired_ = false;
        }
    }
    else if(screen_ == Screen::SampleSynth
            && sample_synth_param_page_ == SampleSynthParamPage::Setup)
    {
        // Hold-to-confirm, same 800ms threshold/button2_long_fired_
        // guard as every other hold-to-confirm action here -- this one
        // is instant (no SD I/O, see ApplySampleSynthBreakDivide()'s
        // own comment) but still gated behind a hold since it's a real
        // structural change to the kit's own pad layout, not a plain
        // scalar knob tweak.
        if(b.Pressed() && b.TimeHeldMs() > 800.f && !button2_long_fired_)
        {
            button2_long_fired_ = true;
            TriggerSampleSynthBreakDivideConfirm();
        }
        if(events.btn2_released)
        {
            if(!button2_long_fired_ && events.btn2_held_ms > 800.f)
                TriggerSampleSynthBreakDivideConfirm();
            button2_long_fired_ = false;
        }
    }
    else if(screen_ == Screen::SampleSynth
            && sample_synth_param_page_ == SampleSynthParamPage::Preset)
    {
        // LOAD-ONLY (see this page's own doc comment) -- no Save/
        // ChoosingSave branch, no Files/New top-level chooser: a short
        // tap from Idle jumps straight to the bank list. Both faces have
        // a bank level in front of their item list (see
        // sample_synth_sound_category_'s own comment in ui.h) -- while
        // sitting at that level there's nothing to hold-load yet
        // (active_count=0 below), only something to drill into, which
        // this function's own short-press branch handles further down.
        // Shared browse convention (every engine's Preset/User page): a
        // tap opens a folder, a tap on a sound LOADS it and stays in the
        // folder so the next sound is one K2 turn + tap away. No hold.
        bool at_category_select = sample_synth_sound_category_ < 0;
        if(events.btn2_released)
        {
            if(save_load_mode_ == SaveLoadMode::BrowsingLoad && !at_category_select
               && sample_synth_sound_count_ > 0)
            {
                if(sample_synth_preset_mode_sounds_)
                    TriggerLoadSampleSynthSound();
                else
                    TriggerLoadSampleSynthKit();
            }
            else if(at_category_select
                    && save_load_mode_ == SaveLoadMode::BrowsingLoad
                    && sample_synth_bank_count_ > 0)
            {
                // Short tap at category-select: "Open" the highlighted
                // category (drill into its own file list) -- same
                // button, same label, as the Idle->BrowsingLoad tap
                // below, just one level deeper.
                sample_synth_sound_category_ = sample_synth_sound_category_cursor_;
                sample_synth_sounds_dirty_   = true; // re-scan this category
                sample_synth_kit_cursor_     = 0;
            }
            else if(save_load_mode_ == SaveLoadMode::Idle)
            {
                if(!PerformanceStore::IsCardPresent())
                    PerformanceStore::Remount();
                else
                {
                    save_load_mode_          = SaveLoadMode::BrowsingLoad;
                    load_browsing_files_     = true; // no "Files vs New" chooser -- straight to the list
                    sample_synth_kit_status_[0] = '\0'; // clear any stale result
                    sample_synth_sound_category_ = -1; // always start at category-select
                }
            }
            button2_long_fired_ = false;
        }
    }
    else if(screen_ == Screen::SampleSynth && sample_synth_param_page_ == SampleSynthParamPage::User)
    {
        // Same Save/Load shape as Ensemble's own Preset page
        // (ui.cpp:982-1034) -- whichever face's own User-preset list,
        // see SampleSynthVoiceUserPreset/SampleSynthDrumUserPreset's
        // own comments.
        bool  is_voice = sample_synth_mode_ == SampleSynthMode::Voice;
        int   count    = is_voice ? sample_synth_voice_user_count_ : sample_synth_drum_user_count_;
        char* status   = is_voice ? sample_synth_voice_user_status_ : sample_synth_drum_user_status_;
        // Saving still needs an 800 ms hold (it can overwrite a file);
        // loading follows the shared browse convention -- a tap loads and
        // stays in the list.
        bool can_confirm_save = save_load_mode_ == SaveLoadMode::ChoosingSave;
        if(b.Pressed() && b.TimeHeldMs() > 800.f && !button2_long_fired_ && can_confirm_save)
        {
            button2_long_fired_ = true;
            if(is_voice)
                TriggerSaveSampleSynthVoiceUserPreset(save_as_new_);
            else
                TriggerSaveSampleSynthDrumUserPreset(save_as_new_);
            save_load_mode_ = SaveLoadMode::Idle;
        }
        if(events.btn2_released)
        {
            if(!button2_long_fired_ && events.btn2_held_ms > 800.f && can_confirm_save)
            {
                if(is_voice)
                    TriggerSaveSampleSynthVoiceUserPreset(save_as_new_);
                else
                    TriggerSaveSampleSynthDrumUserPreset(save_as_new_);
                save_load_mode_ = SaveLoadMode::Idle;
            }
            else if(!button2_long_fired_ && save_load_mode_ == SaveLoadMode::BrowsingLoad
                     && count > 0)
            {
                if(is_voice)
                    TriggerLoadSampleSynthVoiceUserPreset();
                else
                    TriggerLoadSampleSynthDrumUserPreset();
            }
            else if(!button2_long_fired_ && save_load_mode_ == SaveLoadMode::Idle)
            {
                if(!PerformanceStore::IsCardPresent())
                    PerformanceStore::Remount();
                else
                {
                    save_load_mode_ = SaveLoadMode::BrowsingLoad;
                    status[0]       = '\0'; // clear any stale result
                }
            }
            button2_long_fired_ = false;
        }
    }
    else if(screen_ == Screen::Global && global_page_ == GlobalPage::Tempo)
    {
        // Button2 does nothing else on this page -- save the current
        // global settings as the startup default (see
        // TriggerSaveDefaults()/PerformanceStore::SavePrefs()). A hold,
        // not a tap, since this is a deliberate write, same weight as
        // File's hold-to-Load/Status's hold-to-Clear.
        if(b.Pressed() && b.TimeHeldMs() > 800.f && !button2_long_fired_)
        {
            button2_long_fired_ = true;
            TriggerSaveDefaults();
        }
        if(events.btn2_released)
        {
            if(!button2_long_fired_ && events.btn2_held_ms > 800.f)
                TriggerSaveDefaults();
            button2_long_fired_ = false;
        }
    }
    else if(events.btn2_released)
    {
        OnButton2Short();
    }
}

void Ui::OnButton1Short()
{
    switch(screen_)
    {
        case Screen::Home: layers_[cursor_layer_].OnRecordButtonPressed(*tempo_); break;
        case Screen::Layer:
            switch(layer_page_)
            {
                case LayerPage::Status: Cur().OnRecordButtonPressed(*tempo_); break;
                case LayerPage::Speed:
                    Cur().SetSpeed01(0.5f); // quick reset to 1.0x
                    // Without this, the knob (if already "picked up")
                    // would overwrite the reset on the very next
                    // ApplyKnobs() tick with wherever it physically
                    // sits -- re-arm pickup so the reset actually
                    // sticks until the knob is swept back to center.
                    k1_pickup_engaged_[(size_t)KnobContext::LayerSpeed] = false;
                    k1_pickup_raw_[(size_t)KnobContext::LayerSpeed]     = 0.5f;
                    break;
                case LayerPage::Filter:
                {
                    int n = (int)FilterMode::kNumModes;
                    int m = ((int)Cur().GetFilterMode() + 1) % n;
                    Cur().SetFilterMode((FilterMode)m);
                    break;
                }
                case LayerPage::Effect:
                {
                    int n = (int)LayerEffect::kNumEffects;
                    int e = ((int)Cur().GetEffect() + 1) % n;
                    Cur().SetEffect((LayerEffect)e);
                    // SetEffect() already reset the params themselves to
                    // 0 (see its comment) -- but cycling through effects
                    // stays within the same knob context the whole time,
                    // so the generic context-change pickup sync never
                    // fires here on its own. Re-arm both knobs to that
                    // fresh 0, same reasoning as the Speed page's reset
                    // above: without this, an already-picked-up knob
                    // would immediately overwrite the reset with wherever
                    // it physically sits.
                    k1_pickup_engaged_[(size_t)KnobContext::LayerEffect] = false;
                    k2_pickup_engaged_[(size_t)KnobContext::LayerEffect] = false;
                    k1_pickup_raw_[(size_t)KnobContext::LayerEffect]     = 0.f;
                    k2_pickup_raw_[(size_t)KnobContext::LayerEffect]     = 0.f;
                    break;
                }
                default: break;
            }
            break;
        case Screen::Global:
            if(global_page_ == GlobalPage::Tempo)
                tempo_->ToggleMetronome();
            else if(global_page_ == GlobalPage::Filter)
            {
                int n = (int)FilterMode::kNumModes;
                int m = ((int)master_filter_mode_ + 1) % n;
                master_filter_mode_ = (FilterMode)m;
            }
            else if(global_page_ == GlobalPage::Reverb)
                global_fx_target_delay_ = false; // knobs -> Reverb (Size/Bypass Send)
            else if(global_page_ == GlobalPage::ReverbTone)
                global_reverb_tone_target_b_ = false; // knobs -> PreDelay/PreFilter
            else if(global_page_ == GlobalPage::Speed)
            {
                // Cycle Normal -> Scrub -> Freeze -> Normal. Leaving
                // Freeze un-freezes every layer immediately (same
                // "reset on leaving the mode" rule as the page-leave
                // auto-reset below).
                if(speed_transport_mode_ == SpeedTransportMode::Freeze)
                {
                    for(int i = 0; i < num_layers_; i++)
                        layers_[i].SetFreezeActive(false);
                }
                int n = (int)SpeedTransportMode::Freeze + 1;
                speed_transport_mode_ = (SpeedTransportMode)(((int)speed_transport_mode_ + 1) % n);
                if(speed_transport_mode_ == SpeedTransportMode::Freeze)
                {
                    for(int i = 0; i < num_layers_; i++)
                        layers_[i].SetFreezeActive(true);
                }
            }
            else if(global_page_ == GlobalPage::File)
            {
                if(save_load_mode_ == SaveLoadMode::BrowsingLoad && load_browsing_files_)
                {
                    // Back out of the numbered file list to the Files/New
                    // chooser (see load_browsing_files_'s own comment).
                    load_browsing_files_ = false;
                }
                else if(save_load_mode_ == SaveLoadMode::BrowsingLoad && !load_new_selected_)
                {
                    // "Files" is highlighted at the chooser -- drill into
                    // the numbered list (Knob1 now scrolls it directly,
                    // see ApplyKnobs()).
                    load_browsing_files_ = true;
                }
                else if(save_load_mode_ != SaveLoadMode::Idle)
                {
                    // Back -- covers ChoosingSave, and the Load chooser
                    // with "New" highlighted (nothing further to drill
                    // into there). Button2 owns confirming whichever of
                    // Save/Load is currently revealed (see
                    // HandleButton2()), so Button1's tap here is
                    // otherwise always just a way out, same as SD MGMT's
                    // own Back.
                    save_load_mode_ = SaveLoadMode::Idle;
                }
                // No card: this hand-wired socket has no card-detect pin
                // (see PerformanceStore::Remount()'s doc comment), so a
                // card swapped out and back in needs an explicit re-mount
                // attempt -- repurpose Button1 for that instead of Save
                // while there's nothing to save to anyway.
                else if(!PerformanceStore::IsCardPresent())
                {
                    PerformanceStore::Remount();
                    file_slots_dirty_ = true; // re-scan once actually mounted
                }
                else
                {
                    // Reveal the Overwrite/Save New choice -- Button2's
                    // hold now confirms it (see HandleButton2()).
                    // Default to whichever's actually available:
                    // Overwrite only makes sense if something's loaded.
                    save_load_mode_ = SaveLoadMode::ChoosingSave;
                    save_as_new_    = loaded_slot_ < 0;
                }
            }
            else if(global_page_ == GlobalPage::Export)
            {
                if(!PerformanceStore::IsCardPresent())
                    PerformanceStore::Remount();
                else
                    TriggerExport();
            }
            else if(global_page_ == GlobalPage::Granular)
            {
                granular_enabled_ = !granular_enabled_;
                // Force-release any voice this engine's own NoteOn/
                // NoteOff dispatch left held (MIDI dispatch updates
                // held-note state regardless of this toggle -- only
                // Process() itself is gated by it, see AudioCallback())
                // on every flip, not just disabling -- a stale held
                // note from before an OFF->ON flip would otherwise
                // start sounding again the instant Process() resumes,
                // with no NoteOn of its own to explain it. Same real
                // need as CC 123/120 (see AllNotesOff()'s own comment),
                // just reachable without depending on the MIDI
                // controller sending that message at all.
                if(granular_)
                    granular_->AllNotesOff();
            }
            else if(global_page_ == GlobalPage::Dexed)
            {
                dexed_enabled_ = !dexed_enabled_;
                if(dexed_)
                    dexed_->AllNotesOff();
            }
            else if(global_page_ == GlobalPage::Ensemble)
            {
                ensemble_enabled_ = !ensemble_enabled_;
                if(ensemble_)
                    ensemble_->AllNotesOff();
            }
            else if(global_page_ == GlobalPage::SampleSynthVoice)
            {
                sample_synth_voice_enabled_ = !sample_synth_voice_enabled_;
                // Mutual exclusion -- see sample_synth_voice_enabled_'s
                // own comment.
                if(sample_synth_voice_enabled_)
                {
                    sample_synth_drum_enabled_ = false;
                    // Enabling IS "this face is live now" -- switch the
                    // moment it's enabled, not only once you also
                    // happen to drill into the screen (a real reported
                    // gap: enabling played nothing until you entered
                    // Screen::SampleSynth separately, since that's the
                    // only other place this used to get called).
                    SwitchSampleSynthMode(SampleSynthMode::Voice);
                }
                if(sample_synth_)
                    sample_synth_->AllNotesOff();
            }
            else if(global_page_ == GlobalPage::SampleSynthDrum)
            {
                sample_synth_drum_enabled_ = !sample_synth_drum_enabled_;
                if(sample_synth_drum_enabled_)
                {
                    sample_synth_voice_enabled_ = false;
                    SwitchSampleSynthMode(SampleSynthMode::Drum);
                }
                if(sample_synth_)
                    sample_synth_->AllNotesOff();
            }
            else if(global_page_ == GlobalPage::Looper)
            {
                looper_enabled_ = !looper_enabled_;
                // Switching off resets the shared tempo clock's own
                // phase (bar/beat position, count-in state, click
                // envelope) back to bar 1 beat 1 -- see ResetPhase()'s
                // own doc comment. AudioCallback() also stops advancing
                // it at all while disabled (see IsLooperEnabled() there),
                // so this is what makes "off" actually mean "reset and
                // ready to resume cleanly", not just frozen wherever it
                // happened to be.
                //
                // Every layer's own play_pos_ needs the exact same
                // treatment -- AudioCallback() skips LooperLayer::Process()
                // entirely while disabled, so a layer mid-loop simply
                // freezes at whatever sample it was on, not sample 0. Left
                // alone, re-enabling would resume playback from that
                // frozen (often mid-loop) position while the tempo clock
                // above restarts from bar 1 -- audibly and visually out of
                // sync with each other, exactly the real report that
                // prompted this ("counter is not synced" after
                // re-enabling). Resetting both together at disable time is
                // what actually makes "off" mean "stopped", not just
                // "paused mid-loop".
                if(!looper_enabled_)
                {
                    if(tempo_)
                        tempo_->ResetPhase();
                    for(int i = 0; i < num_layers_; i++)
                        layers_[i].SetPlayPosRaw(0.f);
                }
            }
            else if(global_page_ == GlobalPage::SdMgmt)
            {
                if(!sd_mgmt_in_folder_)
                {
                    // Drill into the highlighted folder -- force a fresh
                    // scan (same "re-scan on entry" idiom as Global:File)
                    // since files may have changed since the last visit.
                    sd_mgmt_in_folder_ = true;
                    sd_mgmt_cursor_    = 0;
                    sd_mgmt_status_[0] = '\0';
                    switch(sd_mgmt_folder_)
                    {
                        case SdMgmtFolder::Performances: file_slots_dirty_ = true; break;
                        case SdMgmtFolder::GranularPresets:
                            granular_preset_slots_dirty_ = true;
                            break;
                        case SdMgmtFolder::DexedPresets:
                            dexed_preset_slots_dirty_ = true;
                            break;
                        case SdMgmtFolder::EnsemblePresets:
                            ensemble_preset_slots_dirty_ = true;
                            break;
                        case SdMgmtFolder::SampleSynthCaptures:
                        case SdMgmtFolder::SampleSynthKits:
                            sd_mgmt_bank_        = -1; // bank list first
                            sd_mgmt_banks_dirty_ = true;
                            break;
                        case SdMgmtFolder::SampleSynthVoiceUserPresets:
                            sample_synth_voice_user_dirty_ = true;
                            break;
                        case SdMgmtFolder::SampleSynthDrumUserPresets:
                            sample_synth_drum_user_dirty_ = true;
                            break;
                        default: break;
                    }
                }
                else
                {
                    // Back up one level -- from a bank's items to the
                    // bank list, otherwise to folder-select. Button1's
                    // hold means Duplicate once already browsing files
                    // (see OnButton1Long()), so its tap is free to mean this.
                    if(SdMgmtIsBankFolder() && sd_mgmt_bank_ >= 0)
                    {
                        sd_mgmt_cursor_ = sd_mgmt_bank_;
                        sd_mgmt_bank_   = -1;
                    }
                    else
                        sd_mgmt_in_folder_ = false;
                    sd_mgmt_status_[0] = '\0';
                }
            }
            break;
        case Screen::Granular:
            if(granular_param_page_ == GranularParamPage::Grain)
                granular_grain_target_gap_jitter_ = false; // knobs -> Size+Fill
            else if(granular_param_page_ == GranularParamPage::Mix)
                granular_mix_target_reverb_ = false; // knobs -> Grain+Scan
            else if(granular_param_page_ == GranularParamPage::FX)
                granular_fx_target_delay_ = false; // knobs -> Reverb (Send+Size)
            else if(granular_param_page_ == GranularParamPage::ADSR)
                granular_adsr_target_sr_ = false; // knobs -> Attack+Decay
            else if(granular_param_page_ == GranularParamPage::Tune && granular_)
                granular_->SetGrainFollowsNote(!granular_->GetGrainFollowsNote());
            else if(granular_param_page_ == GranularParamPage::Scan && granular_)
                granular_->ToggleScanBounce();
            else if(granular_param_page_ == GranularParamPage::ScanRange)
                granular_scanrange_target_gap_ = false; // knobs -> Start+End
            else if(granular_param_page_ == GranularParamPage::Position && granular_)
                granular_->CycleRhythm();
            else if(granular_param_page_ == GranularParamPage::Filter && granular_)
            {
                // Same "Button1 cycles" idiom as Layer:Filter/Pad:Filter.
                int n = (int)FilterMode::kNumModes;
                int m = ((int)granular_->GetFilterMode() + 1) % n;
                granular_->SetFilterMode((FilterMode)m);
            }
            else if(granular_param_page_ == GranularParamPage::Capture)
            {
                // Cycles Direct Record -> Layer 1 -> ... -> Layer
                // num_layers_ -> Import -> Direct Record. The final
                // Import(-2) -> Direct(-1) step happens for free on the
                // NEXT tap via plain integer ++ -- only the "just past
                // the last layer" transition needs an explicit redirect.
                granular_capture_source_++;
                if(granular_capture_source_ >= num_layers_)
                    granular_capture_source_ = -2;
                if(granular_capture_source_ == -2)
                    granular_import_files_dirty_ = true; // re-scan IMPORT/ on entry
                granular_capture_status_[0] = '\0'; // stale result from the other source
            }
            else if(granular_param_page_ == GranularParamPage::Trim && granular_)
            {
                // Tap to hear exactly the currently-selected trim range,
                // straight through -- see GranularEngine::PlayTrimPreview()'s
                // own comment. A direct user request, not planned
                // upfront: turning Start/End already live-updates the
                // actual source range, but there was no quick way to
                // just hear it without playing a MIDI note through the
                // grain engine.
                granular_->PlayTrimPreview();
            }
            else if(granular_param_page_ == GranularParamPage::Preset)
            {
                if(save_load_mode_ == SaveLoadMode::BrowsingLoad && load_browsing_files_)
                {
                    // Back out of the numbered list to the Files/New
                    // chooser -- see Global:File's own comment.
                    load_browsing_files_ = false;
                }
                else if(save_load_mode_ == SaveLoadMode::BrowsingLoad && !load_new_selected_)
                {
                    // "Files" is highlighted -- drill into the numbered
                    // list (Knob1 now scrolls it directly).
                    load_browsing_files_ = true;
                }
                else if(save_load_mode_ != SaveLoadMode::Idle)
                {
                    // Back -- Button2 confirms Save/Load (see
                    // HandleButton2()), so this tap always just returns
                    // to the page's own idle state, same as Global:File.
                    save_load_mode_ = SaveLoadMode::Idle;
                }
                else if(!PerformanceStore::IsCardPresent())
                    PerformanceStore::Remount();
                else
                {
                    // Reveal the Overwrite/Save New choice -- Button2's
                    // hold now confirms it (see HandleButton2()).
                    save_load_mode_ = SaveLoadMode::ChoosingSave;
                    save_as_new_    = granular_loaded_preset_slot_ <= 0;
                }
            }
            break;
        case Screen::Dexed:
            if(dexed_param_page_ == DexedParamPage::Algo && dexed_)
            {
                // Button1 cycles backward, Button2 (OnButton2Short())
                // cycles forward -- a two-direction cycle since, unlike
                // Layer:Filter/Granular:Filter's short mode lists,
                // stepping past algorithm 32 back around to 1 (or vice
                // versa) to reach a nearby one is a real, common need.
                int algo = (dexed_->GetPatchByte(134) + 31) % 32;
                dexed_->SetPatchByte(134, (uint8_t)algo);
            }
            else if(dexed_param_page_ == DexedParamPage::Filter && dexed_)
            {
                // Same "Button1 cycles" idiom as Layer:Filter/Granular:Filter.
                int n = (int)FilterMode::kNumModes;
                int m = ((int)dexed_->GetFilterMode() + 1) % n;
                dexed_->SetFilterMode((FilterMode)m);
            }
            else if(dexed_param_page_ == DexedParamPage::FX)
                dexed_fx_target_delay_ = false; // knobs -> Reverb (Send+Size)
            else if(dexed_param_page_ == DexedParamPage::Vibrato && dexed_)
                dexed_->CycleModWheelTarget();
            else if(dexed_param_page_ == DexedParamPage::Preset)
            {
                if(save_load_mode_ == SaveLoadMode::BrowsingLoad && dexed_import_browsing_)
                {
                    // Back out of the flat .syx file list to the
                    // top-level chooser -- one level only, unlike Files'
                    // own several (see DexedFilesGroup's own comment).
                    dexed_import_browsing_ = false;
                }
                else if(save_load_mode_ == SaveLoadMode::BrowsingLoad && load_browsing_files_
                         && dexed_files_group_open_ && dexed_preset_folder_open_)
                {
                    // Inside an open folder (Roms/Dexed/Imports), already
                    // browsing its own presets -- back out to this
                    // group's own folder list. Button2 is "Open" at
                    // every other Files level (see HandleButton2()), but
                    // this one has nothing further to open, so it keeps
                    // its own separate, established job (Load preview/
                    // hold-to-commit) instead.
                    dexed_preset_folder_open_ = false;
                }
                else if(save_load_mode_ == SaveLoadMode::BrowsingLoad && load_browsing_files_
                         && dexed_files_group_open_)
                {
                    // At a group's own folder list (Roms/Dexed/Imports),
                    // or the User group's flat list (which has no folder
                    // level of its own, see DexedFilesGroup's own
                    // comment) -- Back, up to the group chooser. Button2
                    // is "Open" at this level instead (drills into the
                    // highlighted folder).
                    dexed_files_group_open_ = false;
                }
                else if(save_load_mode_ == SaveLoadMode::BrowsingLoad && load_browsing_files_
                         && !dexed_files_group_open_)
                {
                    // At the group chooser -- Back, up to the top
                    // Files/Import/New chooser. Button2 is "Open" here
                    // too (drills into whichever group is highlighted).
                    load_browsing_files_ = false;
                }
                else if(save_load_mode_ == SaveLoadMode::BrowsingLoad && dexed_import_selected_)
                {
                    // "Import" is highlighted -- drill into the flat
                    // DXIMPORT/ file list (Knob1 now scrolls
                    // dexed_import_cursor_ directly), rescanning the
                    // card fresh every time this is opened, same
                    // "re-scan on entry" rule as Grains' own Import.
                    dexed_import_browsing_    = true;
                    dexed_import_files_dirty_ = true;
                    dexed_import_cursor_      = 0;
                }
                else if(save_load_mode_ == SaveLoadMode::BrowsingLoad && !load_new_selected_)
                {
                    // "Files" is highlighted at the TOP chooser -- drill
                    // into the group chooser (Knob1 now scrolls
                    // dexed_files_group_), always starting fresh at Roms.
                    load_browsing_files_       = true;
                    dexed_files_group_open_    = false;
                    dexed_files_group_         = DexedFilesGroup::Roms;
                    dexed_preset_folder_open_  = false;
                    dexed_preset_folder_cursor_ = 0;
                }
                else if(save_load_mode_ != SaveLoadMode::Idle)
                {
                    // Back -- Button2 confirms Save/Load (see
                    // HandleButton2()), so this tap always just returns
                    // to the page's own idle state, same as Global:File.
                    save_load_mode_ = SaveLoadMode::Idle;
                }
                else if(!PerformanceStore::IsCardPresent())
                    PerformanceStore::Remount();
                else
                {
                    // Reveal the Overwrite/Save New choice -- Button2's
                    // hold now confirms it (see HandleButton2()).
                    save_load_mode_ = SaveLoadMode::ChoosingSave;
                    save_as_new_    = dexed_loaded_preset_slot_ <= DexedSynth::GetNumFactoryPresets();
                }
            }
            break;
        case Screen::Ensemble:
            if(ensemble_param_page_ == EnsembleParamPage::Env)
                ensemble_env_target_sr_ = false; // knobs -> Attack+Decay
            else if(ensemble_param_page_ == EnsembleParamPage::FX)
                ensemble_fx_target_delay_ = false; // knobs -> Reverb (Send+Size)
            else if(ensemble_param_page_ == EnsembleParamPage::Filter && ensemble_)
            {
                // Same "Button1 cycles" idiom as Layer:Filter/Dexed:Filter.
                int n = (int)FilterMode::kNumModes;
                int m = ((int)ensemble_->GetFilterMode() + 1) % n;
                ensemble_->SetFilterMode((FilterMode)m);
            }
            else if(ensemble_param_page_ == EnsembleParamPage::MIDI && ensemble_)
                // Same page already shows/sets the MIDI channel -- mod
                // wheel is a MIDI controller too, and Dexed has no
                // equivalent free button on its own MIDI page, so this
                // doesn't collide with any existing convention.
                ensemble_->CycleModWheelTarget();
            else if(ensemble_param_page_ == EnsembleParamPage::Preset)
            {
                if(save_load_mode_ == SaveLoadMode::BrowsingLoad && load_browsing_files_)
                {
                    // Back out of the numbered list to the Files/New
                    // chooser -- see Global:File's own comment.
                    load_browsing_files_ = false;
                }
                else if(save_load_mode_ == SaveLoadMode::BrowsingLoad && !load_new_selected_)
                {
                    // "Files" is highlighted -- drill into the combined
                    // [factory][user] list (Knob1 now scrolls it directly).
                    load_browsing_files_ = true;
                }
                else if(save_load_mode_ != SaveLoadMode::Idle)
                {
                    // Back -- Button2 confirms Save/Load (see
                    // HandleButton2()), so this tap always just returns
                    // to the page's own idle state, same as Global:File.
                    save_load_mode_ = SaveLoadMode::Idle;
                }
                else if(!PerformanceStore::IsCardPresent())
                    PerformanceStore::Remount();
                else
                {
                    // Reveal the Overwrite/Save New choice -- Button2's
                    // hold now confirms it (see HandleButton2()).
                    save_load_mode_ = SaveLoadMode::ChoosingSave;
                    save_as_new_
                        = ensemble_loaded_preset_slot_ <= EnsembleSynth::GetNumFactoryPresets();
                }
            }
            break;
        case Screen::DexedOperator:
        {
            // Button1 cycles which of the 6 operators is being edited --
            // rotate stays reserved for DexedOpParamPage navigation, same
            // "Button1 cycles a non-page selector" convention as Algo's
            // own algorithm cycle above.
            dexed_op_index_ = (dexed_op_index_ + 1) % 6;
            // Real bug, confirmed on hardware: DexedOpRatioLevel/Detune/
            // EgRateAD/etc. are each ONE KnobContext shared across all 6
            // operators (branching internally on dexed_op_index_, same
            // idiom as LayerStatus/cursor_layer_) -- but unlike rotating
            // between LayerStatus's own layers (which always passes back
            // through Home, a genuine context change, first), changing
            // dexed_op_index_ here does NOT change which KnobContext
            // enum value CurrentKnobContext() returns, so ApplyKnobs()'s
            // own automatic "re-arm pickup on context change" never
            // fires. Left alone, an already-engaged knob would
            // immediately re-apply its last raw position against the
            // NEWLY selected operator's bytes on the very next tick --
            // exactly what turning one operator's Rate then cycling to
            // another showed on hardware ("all the other operators'
            // rates change to that value"). Screen::Mixer's own rotate
            // handling hit the identical class of bug for mixer_position_
            // and fixed it the same way: explicitly re-run the reset/
            // reseed here instead of relying on the automatic path.
            KnobContext ctx       = CurrentKnobContext();
            size_t      ctx_index = (size_t)ctx;
            k1_pickup_engaged_[ctx_index] = false;
            k2_pickup_engaged_[ctx_index] = false;
            SyncPickupTargets(ctx);
            last_knob_context_ = ctx;
            break;
        }
        case Screen::Mixer:
            mixer_target_reverb_ = false; // knobs -> Volume+Pan (ignored on Master)
            break;
        case Screen::SampleSynth:
            if(sample_synth_param_page_ == SampleSynthParamPage::Filter && sample_synth_)
            {
                // Same "Button1 cycles" idiom as Layer:Filter/Dexed:Filter.
                int n = (int)FilterMode::kNumModes;
                int m = ((int)sample_synth_->GetFilterMode() + 1) % n;
                sample_synth_->SetFilterMode((FilterMode)m);
            }
            else if(sample_synth_param_page_ == SampleSynthParamPage::Setup && sample_synth_)
            {
                // Per-kit, not global -- see SampleSynthPresetData::
                // mono_choke's own comment. Moved here from Mix once
                // Setup absorbed it alongside Tune/Slices -- Mix's own
                // Button1/Button2 go back to unused.
                sample_synth_->SetMonoChoke(!sample_synth_->GetMonoChoke());
            }
            else if(sample_synth_param_page_ == SampleSynthParamPage::Env)
                sample_synth_adsr_target_sr_ = false; // knobs -> Attack+Decay
            else if(sample_synth_param_page_ == SampleSynthParamPage::FX)
                sample_synth_fx_target_delay_ = false; // knobs -> Reverb (Send+Size)
            else if(sample_synth_param_page_ == SampleSynthParamPage::Capture)
            {
                // Same cycle idiom as GranularParamPage::Capture's own
                // Button1 handling -- Direct Record -> Layer 1 -> ... ->
                // Layer num_layers_ -> Import -> Direct Record.
                sample_synth_capture_source_++;
                if(sample_synth_capture_source_ >= num_layers_)
                    sample_synth_capture_source_ = -2;
                if(sample_synth_capture_source_ == -2)
                    sample_synth_import_files_dirty_ = true; // re-scan IMPORT/ on entry
                sample_synth_capture_status_[0] = '\0'; // stale result from the other source
            }
            else if(sample_synth_param_page_ == SampleSynthParamPage::Trim && sample_synth_)
            {
                // Tap to hear exactly the currently-selected trim range --
                // same gesture/reasoning as GranularParamPage::Trim's own
                // Button1 (see SampleSynth::PlayPreview()'s own comment).
                sample_synth_->PlayPreview();
            }
            else if(sample_synth_param_page_ == SampleSynthParamPage::Tune && sample_synth_)
            {
                // Mono/Poly for the CAPTURED wav specifically -- see
                // sample_synth_cap_mono_'s own comment. Replaces the
                // old "toggle chromatic on/off" (captures are always
                // chromatic now, per direct request).
                sample_synth_cap_mono_ = !sample_synth_cap_mono_;
                if(sample_synth_voice_source_ == SampleSynthVoiceSource::Captured)
                    sample_synth_->SetChromaticMono(sample_synth_cap_mono_);
            }
            else if(sample_synth_param_page_ == SampleSynthParamPage::Import)
            {
                int n = (int)SampleSynthImportTarget::kCount;
                int p = (((int)sample_synth_import_target_ + 1) % n + n) % n;
                sample_synth_import_target_        = (SampleSynthImportTarget)p;
                sample_synth_pack_import_status_[0] = '\0'; // stale result from the other target
            }
            else if(sample_synth_param_page_ == SampleSynthParamPage::Preset)
            {
                if(save_load_mode_ != SaveLoadMode::Idle)
                {
                    // Back one level: a folder's sounds -> the folder
                    // list (K1 still where it was, so the same folder
                    // stays highlighted) -> out of the browser.
                    if(sample_synth_sound_category_ >= 0)
                        sample_synth_sound_category_ = -1;
                    else
                        save_load_mode_ = SaveLoadMode::Idle;
                }
                else if(!PerformanceStore::IsCardPresent())
                    PerformanceStore::Remount();
                // Idle + card present: Button1 used to toggle between
                // browsing Kits and Sounds here -- no longer a user
                // choice now that SMPVO/SMPDR each only ever browse one
                // (sample_synth_preset_mode_sounds_ follows
                // sample_synth_mode_ directly, see that field's own
                // comment), so Button1 is unused on this page now.
                // Button2 is the only way into BrowsingLoad itself
                // (reveals whichever list is active), no "Save" to
                // reveal -- see this page's own doc comment.
            }
            else if(sample_synth_param_page_ == SampleSynthParamPage::User)
            {
                // Same Save/Load shape Ensemble's own Preset page uses
                // (ui.cpp:982-1034) -- no factory range and no "New"
                // concept here (same reasoning Preset's own Kits/Sounds
                // lists already have none), so this is the simpler
                // flat-list version of that same idiom.
                if(save_load_mode_ != SaveLoadMode::Idle)
                {
                    save_load_mode_ = SaveLoadMode::Idle; // Back, same as every other Preset page
                }
                else if(!PerformanceStore::IsCardPresent())
                    PerformanceStore::Remount();
                else
                {
                    // Reveal the Overwrite/Save New choice -- Button2's
                    // hold confirms it (see the hold-to-confirm block
                    // for this page).
                    save_load_mode_ = SaveLoadMode::ChoosingSave;
                    save_as_new_    = sample_synth_mode_ == SampleSynthMode::Voice
                                          ? sample_synth_voice_user_loaded_slot_ <= 0
                                          : sample_synth_drum_user_loaded_slot_ <= 0;
                }
            }
            break;
    }
}

void Ui::OnButton1Long()
{
    // Long-press is only meaningful on the transport (arm to record /
    // start overdub) and SD MGMT's own hold-to-duplicate -- File/Grains
    // Preset don't use Button1's hold for anything any more (New is
    // reachable from the Load browse list's own trailing "New" entry
    // instead, see TriggerLoad()/TriggerNewGranularPreset()), so a long
    // hold there does nothing.
    if(screen_ == Screen::Home)
        layers_[cursor_layer_].OnRecordButtonLongPress(*tempo_);
    else if(screen_ == Screen::Layer && layer_page_ == LayerPage::Status)
        Cur().OnRecordButtonLongPress(*tempo_);
    else if(screen_ == Screen::Global && global_page_ == GlobalPage::SdMgmt
            && sd_mgmt_in_folder_ && !(SdMgmtIsBankFolder() && sd_mgmt_bank_ < 0))
        TriggerSdMgmtDuplicate();
}

void Ui::OnButton1Release()
{
    if(screen_ == Screen::Home)
        layers_[cursor_layer_].OnRecordButtonReleased();
    else if(screen_ == Screen::Layer && layer_page_ == LayerPage::Status)
        Cur().OnRecordButtonReleased();
}

void Ui::OnButton2Short()
{
    if(screen_ == Screen::Home)
        bypass_ = !bypass_;
    else if(screen_ == Screen::Global && global_page_ == GlobalPage::Reverb)
        global_fx_target_delay_ = true; // knobs -> Delay (Time/Feedback)
    else if(screen_ == Screen::Global && global_page_ == GlobalPage::ReverbTone)
        global_reverb_tone_target_b_ = true; // knobs -> DecayDiffusion/Damping
    else if(screen_ == Screen::Global && global_page_ == GlobalPage::Speed)
    {
        SetProjectSpeed01(0.5f); // tap = reset to 1.0x, mirrors Layer:Speed's Button1 tap
        // Re-arm pickup so the reset actually sticks, same reasoning as
        // Layer:Speed's own reset above.
        k1_pickup_engaged_[(size_t)KnobContext::GlobalSpeed] = false;
        k1_pickup_raw_[(size_t)KnobContext::GlobalSpeed]     = 0.5f;
    }
    else if(screen_ == Screen::Global && global_page_ == GlobalPage::Export)
        TriggerExportMicroDexed();
    else if(screen_ == Screen::Dexed && dexed_param_page_ == DexedParamPage::Algo && dexed_)
    {
        // Forward direction of the Algo page's two-way cycle -- see
        // OnButton1Short()'s own comment for why this page (uniquely)
        // gets both directions.
        int algo = (dexed_->GetPatchByte(134) + 1) % 32;
        dexed_->SetPatchByte(134, (uint8_t)algo);
    }
    else if(screen_ == Screen::Dexed && dexed_param_page_ == DexedParamPage::FX)
        dexed_fx_target_delay_ = true; // knobs -> Delay (Send+Time)
    else if(screen_ == Screen::Dexed && dexed_param_page_ == DexedParamPage::Vibrato)
        // Toggles (not just sets true) -- same reasoning as
        // DexedOpParamPage::EgRate's own Button2 above: Button1 on this
        // page is already committed to cycling the mod wheel's target,
        // so there's no second button free to select Speed/Depth
        // explicitly; Button2 alone has to cover both directions.
        dexed_vibrato_target_sens_ = !dexed_vibrato_target_sens_;
    else if(screen_ == Screen::DexedOperator && dexed_op_page_ == DexedOpParamPage::EgRate)
        // Toggles (not just sets true) -- unlike Granular's own ADSR
        // page, Button1 is already committed to operator-select duty
        // here (see OnButton1Short()), so there's no second button free
        // to select AD explicitly; Button2 alone has to cover both
        // directions.
        dexed_op_egrate_target_sr_ = !dexed_op_egrate_target_sr_;
    else if(screen_ == Screen::DexedOperator && dexed_op_page_ == DexedOpParamPage::EgLevel)
        dexed_op_eglevel_target_sr_ = !dexed_op_eglevel_target_sr_;
    else if(screen_ == Screen::Granular && granular_param_page_ == GranularParamPage::Grain)
        granular_grain_target_gap_jitter_ = true; // knobs -> Gap+Jitter
    else if(screen_ == Screen::Granular && granular_param_page_ == GranularParamPage::Position
            && granular_)
        granular_->CycleGrainSpeed();
    else if(screen_ == Screen::Granular && granular_param_page_ == GranularParamPage::Scan
            && granular_)
        granular_->CycleScanDirection();
    else if(screen_ == Screen::Granular && granular_param_page_ == GranularParamPage::ScanRange)
        granular_scanrange_target_gap_ = true; // knob -> Gap
    else if(screen_ == Screen::Granular && granular_param_page_ == GranularParamPage::ADSR)
        granular_adsr_target_sr_ = true; // knobs -> Sustain/Release
    else if(screen_ == Screen::Granular && granular_param_page_ == GranularParamPage::Mix)
        granular_mix_target_reverb_ = true; // knobs -> Reverb Send
    else if(screen_ == Screen::Granular && granular_param_page_ == GranularParamPage::FX)
        granular_fx_target_delay_ = true; // knobs -> Delay (Send+Time)
    else if(screen_ == Screen::Ensemble && ensemble_param_page_ == EnsembleParamPage::Env)
        ensemble_env_target_sr_ = true; // knobs -> Sustain+Release
    else if(screen_ == Screen::Ensemble && ensemble_param_page_ == EnsembleParamPage::FX)
        ensemble_fx_target_delay_ = true; // knobs -> Delay (Send+Time)
    else if(screen_ == Screen::SampleSynth && sample_synth_param_page_ == SampleSynthParamPage::Env)
        sample_synth_adsr_target_sr_ = true; // knobs -> Sustain+Release
    else if(screen_ == Screen::SampleSynth && sample_synth_param_page_ == SampleSynthParamPage::FX)
        sample_synth_fx_target_delay_ = true; // knobs -> Delay (Send+Time)
    else if(screen_ == Screen::SampleSynth && sample_synth_param_page_ == SampleSynthParamPage::Tune
             && sample_synth_)
    {
        // Mono/Poly for whatever's loaded via Import/Preset/User
        // specifically -- see sample_synth_imp_mono_'s own comment.
        sample_synth_imp_mono_ = !sample_synth_imp_mono_;
        if(sample_synth_voice_source_ == SampleSynthVoiceSource::Imported)
            sample_synth_->SetChromaticMono(sample_synth_imp_mono_);
    }
    else if(screen_ == Screen::Mixer)
        mixer_target_reverb_ = true; // knobs -> Reverb Send (ignored on Master)
    // File's and Pad Preset's short-tap behavior (Save As New) is
    // handled directly in HandleButton2() instead, alongside their own
    // hold-to-Load logic -- see there. All other screens: Button2 is
    // unused (Status page's Button2 is handled separately in
    // HandleButton2 as a hold-to-confirm clear).
}

Ui::KnobContext Ui::CurrentKnobContext() const
{
    switch(screen_)
    {
        case Screen::Home: return KnobContext::Home;
        case Screen::Layer:
            switch(layer_page_)
            {
                case LayerPage::Status: return KnobContext::LayerStatus;
                case LayerPage::Speed: return KnobContext::LayerSpeed;
                case LayerPage::Filter: return KnobContext::LayerFilter;
                case LayerPage::Effect: return KnobContext::LayerEffect;
                case LayerPage::Reverb: return KnobContext::LayerReverb;
                case LayerPage::Gain: return KnobContext::LayerGain;
                default: return KnobContext::LayerStatus;
            }
        case Screen::Global:
            switch(global_page_)
            {
                case GlobalPage::Tempo: return KnobContext::GlobalTempo;
                case GlobalPage::Filter: return KnobContext::GlobalFilter;
                case GlobalPage::Reverb:
                    return global_fx_target_delay_ ? KnobContext::GlobalDelay
                                                      : KnobContext::GlobalReverb;
                case GlobalPage::ReverbTone:
                    return global_reverb_tone_target_b_
                               ? KnobContext::GlobalReverbToneDecayDamping
                               : KnobContext::GlobalReverbTonePreDelay;
                case GlobalPage::ReverbDiffusion: return KnobContext::GlobalReverbDiffusion;
                case GlobalPage::Speed: return KnobContext::GlobalSpeed;
                case GlobalPage::File: return KnobContext::GlobalFile;
                case GlobalPage::Export: return KnobContext::GlobalExport;
                case GlobalPage::Granular: return KnobContext::GlobalGranular;
                case GlobalPage::Looper: return KnobContext::GlobalLooper;
                case GlobalPage::Mixer: return KnobContext::GlobalMixer;
                case GlobalPage::SdMgmt: return KnobContext::GlobalSdMgmt;
                case GlobalPage::Dexed: return KnobContext::GlobalDexed;
                case GlobalPage::Ensemble: return KnobContext::GlobalEnsemble;
                case GlobalPage::SampleSynthVoice:
                case GlobalPage::SampleSynthDrum: return KnobContext::GlobalSampleSynth;
                default: return KnobContext::GlobalTempo;
            }
        case Screen::Dexed:
            switch(dexed_param_page_)
            {
                case DexedParamPage::Algo: return KnobContext::DexedAlgo;
                case DexedParamPage::Feedback: return KnobContext::DexedFeedback;
                case DexedParamPage::Vibrato:
                    return dexed_vibrato_target_sens_ ? KnobContext::DexedVibratoSens
                                                          : KnobContext::DexedVibrato;
                case DexedParamPage::Brightness: return KnobContext::DexedBrightness;
                case DexedParamPage::EnvSpeed: return KnobContext::DexedEnvSpeed;
                case DexedParamPage::Filter: return KnobContext::DexedFilter;
                case DexedParamPage::Mix: return KnobContext::DexedMix;
                case DexedParamPage::FX:
                    return dexed_fx_target_delay_ ? KnobContext::DexedFxDelay
                                                     : KnobContext::DexedFxReverb;
                case DexedParamPage::Fold: return KnobContext::DexedFold;
                case DexedParamPage::MIDI: return KnobContext::DexedMidi;
                case DexedParamPage::Advanced: return KnobContext::DexedAdvanced;
                case DexedParamPage::Preset: return KnobContext::DexedPreset;
                default: return KnobContext::DexedAlgo;
            }
        case Screen::DexedOperator:
            switch(dexed_op_page_)
            {
                case DexedOpParamPage::Algo: return KnobContext::DexedOpAlgo;
                case DexedOpParamPage::RatioLevel: return KnobContext::DexedOpRatioLevel;
                case DexedOpParamPage::Detune: return KnobContext::DexedOpDetune;
                case DexedOpParamPage::EgRate:
                    return dexed_op_egrate_target_sr_ ? KnobContext::DexedOpEgRateSR
                                                          : KnobContext::DexedOpEgRateAD;
                case DexedOpParamPage::EgLevel:
                    return dexed_op_eglevel_target_sr_ ? KnobContext::DexedOpEgLevelSR
                                                           : KnobContext::DexedOpEgLevelAD;
                case DexedOpParamPage::AmpModSens: return KnobContext::DexedOpAmpModSens;
                case DexedOpParamPage::VelocitySens: return KnobContext::DexedOpVelocitySens;
                case DexedOpParamPage::KeybRateScale: return KnobContext::DexedOpKeybRateScale;
                default: return KnobContext::DexedOpRatioLevel;
            }
        case Screen::Ensemble:
            switch(ensemble_param_page_)
            {
                case EnsembleParamPage::Registration: return KnobContext::EnsembleRegistration;
                case EnsembleParamPage::Env:
                    return ensemble_env_target_sr_ ? KnobContext::EnsembleEnvSR
                                                      : KnobContext::EnsembleEnvAD;
                case EnsembleParamPage::Ensemble: return KnobContext::EnsembleEnsemble;
                case EnsembleParamPage::Filter: return KnobContext::EnsembleFilter;
                case EnsembleParamPage::Mix: return KnobContext::EnsembleMix;
                case EnsembleParamPage::FX:
                    return ensemble_fx_target_delay_ ? KnobContext::EnsembleFxDelay
                                                        : KnobContext::EnsembleFxReverb;
                case EnsembleParamPage::MIDI: return KnobContext::EnsembleMidi;
                case EnsembleParamPage::Preset: return KnobContext::EnsemblePreset;
                default: return KnobContext::EnsembleRegistration;
            }
        case Screen::SampleSynth:
            switch(sample_synth_param_page_)
            {
                case SampleSynthParamPage::Filter: return KnobContext::SampleSynthFilter;
                case SampleSynthParamPage::Env:
                    return sample_synth_adsr_target_sr_ ? KnobContext::SampleSynthEnvSR
                                                            : KnobContext::SampleSynthEnvAD;
                case SampleSynthParamPage::Mix: return KnobContext::SampleSynthMix;
                case SampleSynthParamPage::Setup: return KnobContext::SampleSynthSetup;
                case SampleSynthParamPage::FX:
                    return sample_synth_fx_target_delay_ ? KnobContext::SampleSynthFxDelay
                                                             : KnobContext::SampleSynthFxReverb;
                case SampleSynthParamPage::MIDI: return KnobContext::SampleSynthMidi;
                case SampleSynthParamPage::Capture: return KnobContext::SampleSynthCapture;
                case SampleSynthParamPage::Trim: return KnobContext::SampleSynthTrim;
                case SampleSynthParamPage::Tune: return KnobContext::SampleSynthTune;
                case SampleSynthParamPage::Import: return KnobContext::SampleSynthImport;
                case SampleSynthParamPage::Preset: return KnobContext::SampleSynthPreset;
                case SampleSynthParamPage::User: return KnobContext::SampleSynthUser;
                default: return KnobContext::SampleSynthFilter;
            }
        case Screen::Granular:
            switch(granular_param_page_)
            {
                case GranularParamPage::Grain:
                    return granular_grain_target_gap_jitter_ ? KnobContext::GranularGrainGapJitter
                                                              : KnobContext::GranularGrainSizeFill;
                case GranularParamPage::Position: return KnobContext::GranularPosition;
                case GranularParamPage::ScanRange:
                    return granular_scanrange_target_gap_ ? KnobContext::GranularScanGap
                                                              : KnobContext::GranularScanRange;
                case GranularParamPage::Scan: return KnobContext::GranularScan;
                case GranularParamPage::Tune:
                    return KnobContext::GranularTune;
                case GranularParamPage::ADSR:
                    return granular_adsr_target_sr_ ? KnobContext::GranularEnvSR
                                                       : KnobContext::GranularEnvAD;
                case GranularParamPage::Filter: return KnobContext::GranularFilter;
                case GranularParamPage::Mix:
                    return granular_mix_target_reverb_ ? KnobContext::GranularMixReverb
                                                          : KnobContext::GranularMix;
                case GranularParamPage::FX:
                    return granular_fx_target_delay_ ? KnobContext::GranularFxDelay
                                                        : KnobContext::GranularFxReverb;
                case GranularParamPage::MIDI: return KnobContext::GranularMidi;
                case GranularParamPage::Capture: return KnobContext::GranularCapture;
                case GranularParamPage::Trim: return KnobContext::GranularTrim;
                case GranularParamPage::Preset: return KnobContext::GranularPreset;
                default: return KnobContext::GranularGrainSizeFill;
            }
        case Screen::Mixer:
            if(mixer_position_ >= kNumMixerChannels) // Scope stop
                return KnobContext::MixerNoKnobs;
            if(mixer_position_ == kNumMixerChannels - 1) // Master
                return KnobContext::MixerMaster;
            return mixer_target_reverb_ ? KnobContext::MixerReverb : KnobContext::MixerVolPan;
    }
    return KnobContext::Home;
}

void Ui::SyncPickupTargets(KnobContext ctx)
{
    size_t i = (size_t)ctx;
    switch(ctx)
    {
        case KnobContext::Home:
            k1_pickup_raw_[i] = master_volume01_;
            k2_pickup_raw_[i] = tempo_->GetMetronomeVolume01();
            break;
        case KnobContext::LayerStatus:
            k1_pickup_raw_[i] = Cur().GetVolume01();
            k2_pickup_raw_[i] = Cur().GetPan01();
            break;
        case KnobContext::LayerSpeed:
            k1_pickup_raw_[i] = Cur().GetSpeed01();
            break;
        case KnobContext::LayerFilter:
            k1_pickup_raw_[i] = Cur().GetFilterCutoff01();
            k2_pickup_raw_[i] = Cur().GetFilterResonance01();
            break;
        case KnobContext::LayerEffect:
            k1_pickup_raw_[i] = Cur().GetEffectParamA01();
            k2_pickup_raw_[i] = Cur().GetEffectParamB01();
            break;
        case KnobContext::LayerReverb:
            k1_pickup_raw_[i] = Cur().GetReverbSend01();
            k2_pickup_raw_[i] = Cur().GetDelaySend01();
            break;
        case KnobContext::LayerGain:
            k1_pickup_raw_[i] = Cur().GetInputGain01();
            break;
        case KnobContext::GlobalTempo:
            // No raw01 storage for BPM/Bars (SetBpm/SetBars take the
            // actual value directly, unlike every LooperLayer control) --
            // invert their known forward mappings from ApplyKnobs() below.
            // Bars' inverse uses the bucket midpoint since its forward
            // map floors to an integer (a whole range of k2 produces the
            // same Bars value); BPM's is exact (SetBpm is continuous).
            // Bars' own span is TempoClock::MaxBarsForBpm(bpm) -- not a
            // flat 16 -- so turning the knob fully clockwise always lands
            // on whatever's actually reachable at the current BPM, see
            // ApplyKnobs()'s matching forward map.
            k1_pickup_raw_[i] = Clampf((tempo_->GetBpm() - 50.f) / 190.f, 0.f, 1.f);
            {
                // -0.01 matches the forward map's own epsilon (see
                // ApplyKnobs() below) so k2==1.0 still floors to exactly
                // max_bars, not max_bars+1.
                float max_bars_span = (float)TempoClock::MaxBarsForBpm(tempo_->GetBpm()) - 0.01f;
                k2_pickup_raw_[i] = Clampf(
                    ((float)(tempo_->GetBars() - 1) + 0.5f) / max_bars_span, 0.f, 1.f);
            }
            break;
        case KnobContext::GlobalFilter:
            k1_pickup_raw_[i] = master_filter_cutoff01_;
            k2_pickup_raw_[i] = master_filter_res01_;
            break;
        case KnobContext::GlobalReverb:
            k1_pickup_raw_[i] = reverb_size01_;
            k2_pickup_raw_[i] = bypass_reverb_send01_;
            break;
        case KnobContext::GlobalDelay:
            k1_pickup_raw_[i] = delay_time01_;
            k2_pickup_raw_[i] = delay_feedback01_;
            break;
        case KnobContext::GlobalReverbTonePreDelay:
            k1_pickup_raw_[i] = reverb_predelay01_;
            k2_pickup_raw_[i] = reverb_prefilter01_;
            break;
        case KnobContext::GlobalReverbToneDecayDamping:
            k1_pickup_raw_[i] = reverb_decay_diffusion01_;
            k2_pickup_raw_[i] = reverb_damping01_;
            break;
        case KnobContext::GlobalReverbDiffusion:
            k1_pickup_raw_[i] = reverb_input_diffusion1_01_;
            k2_pickup_raw_[i] = reverb_input_diffusion2_01_;
            break;
        case KnobContext::GlobalSpeed:
            k1_pickup_raw_[i] = project_speed01_;
            break;
        case KnobContext::GlobalFile: break; // browses a list directly, no pickup used
        case KnobContext::GlobalExport: break; // no continuous knob values, Button1 triggers it
        case KnobContext::GlobalGranular: break; // entry point only -- see Screen::Granular instead
        case KnobContext::GlobalLooper: break; // no continuous knobs, Button1 toggle only
        case KnobContext::GlobalMixer: break; // entry point only -- see Screen::Mixer instead
        case KnobContext::GlobalSdMgmt: break; // browses a list directly, no pickup used
        case KnobContext::GlobalDexed: break; // entry point only -- see Screen::Dexed instead
        case KnobContext::GlobalEnsemble: break; // entry point only -- see Screen::Ensemble instead
        case KnobContext::DexedAlgo:
            if(dexed_)
                k1_pickup_raw_[i] = (float)dexed_->GetPatchByte(134) / 32.f;
            break;
        case KnobContext::DexedFeedback:
            if(dexed_)
                k1_pickup_raw_[i] = (float)dexed_->GetPatchByte(135) / 7.f;
            break;
        case KnobContext::DexedVibrato:
            if(dexed_)
            {
                k1_pickup_raw_[i] = (float)dexed_->GetPatchByte(137) / 99.f;
                k2_pickup_raw_[i] = (float)dexed_->GetPatchByte(139) / 99.f;
            }
            break;
        case KnobContext::DexedVibratoSens:
            if(dexed_)
                k1_pickup_raw_[i] = (float)(dexed_->GetPatchByte(143) & 7) / 7.f;
            break;
        case KnobContext::DexedBrightness:
            if(dexed_)
                k1_pickup_raw_[i] = dexed_->GetBrightness01();
            break;
        case KnobContext::DexedEnvSpeed:
            if(dexed_)
                k1_pickup_raw_[i] = dexed_->GetEnvSpeed01();
            break;
        case KnobContext::DexedFilter:
            if(dexed_)
            {
                k1_pickup_raw_[i] = dexed_->GetFilterCutoff01();
                k2_pickup_raw_[i] = dexed_->GetFilterResonance01();
            }
            break;
        case KnobContext::DexedMix:
            if(dexed_)
            {
                k1_pickup_raw_[i] = dexed_->GetReverbSend01();
                k2_pickup_raw_[i] = dexed_->GetOutputLevel01();
            }
            break;
        case KnobContext::DexedFxReverb:
            if(dexed_)
            {
                k1_pickup_raw_[i] = dexed_->GetReverbSend01();
                k2_pickup_raw_[i] = reverb_size01_;
            }
            break;
        case KnobContext::DexedFxDelay:
            if(dexed_)
            {
                k1_pickup_raw_[i] = dexed_->GetDelaySend01();
                k2_pickup_raw_[i] = delay_time01_;
            }
            break;
        case KnobContext::DexedFold:
            if(dexed_)
            {
                k1_pickup_raw_[i] = dexed_->GetFoldAmount01();
                k2_pickup_raw_[i] = dexed_->GetFoldTrim01();
            }
            break;
        case KnobContext::DexedMidi:
            // -1 (Omni) .. 15 (channel 16) -- same bucket-midpoint inverse
            // idiom as GlobalTempo's own Bars knob, just an 18-wide span
            // (17 buckets - 0.01, matching that same epsilon convention)
            // starting at -1 instead of 1.
            k1_pickup_raw_[i] = Clampf(((float)(dexed_midi_channel_ + 1) + 0.5f) / 16.99f, 0.f, 1.f);
            break;
        case KnobContext::DexedAdvanced: break; // entry point only -- see Screen::DexedOperator instead
        case KnobContext::DexedPreset: break; // browses a list directly, no pickup used
        case KnobContext::DexedOpAlgo: break; // view-only here -- see its own comment
        case KnobContext::DexedOpRatioLevel:
            if(dexed_)
            {
                int base = dexed_op_index_ * 21;
                k1_pickup_raw_[i] = (float)dexed_->GetPatchByte(base + 18) / 32.f; // coarse
                k2_pickup_raw_[i] = (float)dexed_->GetPatchByte(base + 16) / 99.f; // output level
            }
            break;
        case KnobContext::DexedOpDetune:
            if(dexed_)
            {
                int base = dexed_op_index_ * 21;
                k1_pickup_raw_[i] = (float)dexed_->GetPatchByte(base + 20) / 14.f;
            }
            break;
        case KnobContext::DexedOpEgRateAD:
            if(dexed_)
            {
                int base = dexed_op_index_ * 21;
                k1_pickup_raw_[i] = (float)dexed_->GetPatchByte(base + 0) / 99.f; // Rate1
                k2_pickup_raw_[i] = (float)dexed_->GetPatchByte(base + 1) / 99.f; // Rate2
            }
            break;
        case KnobContext::DexedOpEgRateSR:
            if(dexed_)
            {
                int base = dexed_op_index_ * 21;
                k1_pickup_raw_[i] = (float)dexed_->GetPatchByte(base + 2) / 99.f; // Rate3
                k2_pickup_raw_[i] = (float)dexed_->GetPatchByte(base + 3) / 99.f; // Rate4
            }
            break;
        case KnobContext::DexedOpEgLevelAD:
            if(dexed_)
            {
                int base = dexed_op_index_ * 21;
                k1_pickup_raw_[i] = (float)dexed_->GetPatchByte(base + 4) / 99.f; // Level1
                k2_pickup_raw_[i] = (float)dexed_->GetPatchByte(base + 5) / 99.f; // Level2
            }
            break;
        case KnobContext::DexedOpEgLevelSR:
            if(dexed_)
            {
                int base = dexed_op_index_ * 21;
                k1_pickup_raw_[i] = (float)dexed_->GetPatchByte(base + 6) / 99.f; // Level3
                k2_pickup_raw_[i] = (float)dexed_->GetPatchByte(base + 7) / 99.f; // Level4
            }
            break;
        case KnobContext::DexedOpAmpModSens:
            if(dexed_)
            {
                int base = dexed_op_index_ * 21;
                k1_pickup_raw_[i] = (float)(dexed_->GetPatchByte(base + 14) & 3) / 3.f;
            }
            break;
        case KnobContext::DexedOpVelocitySens:
            if(dexed_)
            {
                int base = dexed_op_index_ * 21;
                k1_pickup_raw_[i] = (float)dexed_->GetPatchByte(base + 15) / 7.f;
            }
            break;
        case KnobContext::DexedOpKeybRateScale:
            if(dexed_)
            {
                int base = dexed_op_index_ * 21;
                k1_pickup_raw_[i] = (float)dexed_->GetPatchByte(base + 13) / 7.f;
            }
            break;
        case KnobContext::EnsembleRegistration:
            // K1 browses drawbars directly (no pickup -- see
            // ApplyKnobs()'s own comment); only K2's pickup (the
            // currently-selected drawbar's own level) needs seeding.
            if(ensemble_)
                k2_pickup_raw_[i] = ensemble_->GetDrawbarLevel(ensemble_drawbar_index_);
            break;
        case KnobContext::EnsembleEnvAD:
            if(ensemble_)
            {
                k1_pickup_raw_[i] = ensemble_->GetAttack01();
                k2_pickup_raw_[i] = ensemble_->GetDecay01();
            }
            break;
        case KnobContext::EnsembleEnvSR:
            if(ensemble_)
            {
                k1_pickup_raw_[i] = ensemble_->GetSustain01();
                k2_pickup_raw_[i] = ensemble_->GetRelease01();
            }
            break;
        case KnobContext::EnsembleEnsemble:
            if(ensemble_)
            {
                k1_pickup_raw_[i] = ensemble_->GetChorusDepth01();
                k2_pickup_raw_[i] = ensemble_->GetChorusRate01();
            }
            break;
        case KnobContext::EnsembleFilter:
            if(ensemble_)
            {
                k1_pickup_raw_[i] = ensemble_->GetFilterCutoff01();
                k2_pickup_raw_[i] = ensemble_->GetFilterResonance01();
            }
            break;
        case KnobContext::EnsembleMix:
            if(ensemble_)
            {
                k1_pickup_raw_[i] = ensemble_->GetOutputLevel01();
                k2_pickup_raw_[i] = ensemble_->GetPan01();
            }
            break;
        case KnobContext::EnsembleFxReverb:
            if(ensemble_)
            {
                k1_pickup_raw_[i] = ensemble_->GetReverbSend01();
                k2_pickup_raw_[i] = reverb_size01_;
            }
            break;
        case KnobContext::EnsembleFxDelay:
            if(ensemble_)
            {
                k1_pickup_raw_[i] = ensemble_->GetDelaySend01();
                k2_pickup_raw_[i] = delay_time01_;
            }
            break;
        case KnobContext::EnsembleMidi:
            // Same bucket-midpoint idiom as KnobContext::DexedMidi above.
            k1_pickup_raw_[i]
                = Clampf(((float)(ensemble_midi_channel_ + 1) + 0.5f) / 16.99f, 0.f, 1.f);
            break;
        case KnobContext::EnsemblePreset: break; // browses a list directly, no pickup used
        case KnobContext::SampleSynthFilter:
            if(sample_synth_)
            {
                k1_pickup_raw_[i] = sample_synth_->GetFilterCutoff01();
                k2_pickup_raw_[i] = sample_synth_->GetFilterResonance01();
            }
            break;
        case KnobContext::SampleSynthEnvAD:
            if(sample_synth_)
            {
                k1_pickup_raw_[i] = sample_synth_->GetAttack01();
                k2_pickup_raw_[i] = sample_synth_->GetDecay01();
            }
            break;
        case KnobContext::SampleSynthEnvSR:
            if(sample_synth_)
            {
                k1_pickup_raw_[i] = sample_synth_->GetSustain01();
                k2_pickup_raw_[i] = sample_synth_->GetRelease01();
            }
            break;
        case KnobContext::SampleSynthMix:
            if(sample_synth_)
            {
                k1_pickup_raw_[i] = sample_synth_->GetOutputLevel01();
                k2_pickup_raw_[i] = sample_synth_->GetPan01();
            }
            break;
        case KnobContext::SampleSynthSetup:
            // Same bucket-center inverse as SampleSynthTune's own case
            // above -- K1 (Tune) stores the semitone value directly
            // (not a root_note to subtract from), so the inverse is
            // simpler. K2 (Slices) is just a plain 0-16 linear knob.
            k1_pickup_raw_[i] = (float)(sample_synth_setup_tune_semis_ + 24) / 48.f;
            k2_pickup_raw_[i] = (float)sample_synth_setup_pending_slices_ / 16.f;
            break;
        case KnobContext::SampleSynthFxReverb:
            if(sample_synth_)
            {
                k1_pickup_raw_[i] = sample_synth_->GetReverbSend01();
                k2_pickup_raw_[i] = reverb_size01_;
            }
            break;
        case KnobContext::SampleSynthFxDelay:
            if(sample_synth_)
            {
                k1_pickup_raw_[i] = sample_synth_->GetDelaySend01();
                k2_pickup_raw_[i] = delay_time01_;
            }
            break;
        case KnobContext::SampleSynthMidi:
            // Same bucket-midpoint inverse idiom as KnobContext::DexedMidi.
            k1_pickup_raw_[i]
                = Clampf(((float)(sample_synth_midi_channel_ + 1) + 0.5f) / 16.99f, 0.f, 1.f);
            break;
        case KnobContext::SampleSynthCapture: break; // no continuous knobs, Button1/Button2 only
        case KnobContext::SampleSynthTrim:
            k1_pickup_raw_[i] = sample_synth_trim_start01_;
            k2_pickup_raw_[i] = sample_synth_trim_end01_;
            break;
        case KnobContext::SampleSynthTune:
        {
            // Same bucket-center inverse as GranularEngine::
            // GetGrainTuneSemitones01() -- see this page's own
            // ApplyKnobs() case for the forward direction, and that
            // function's own comment for why landing on the bucket's
            // exact edge (instead of its center) caused a real
            // round-trip bug there.
            int semis = 60 - (int)sample_synth_tune_root_note_;
            k1_pickup_raw_[i] = (float)(semis + 24) / 48.f;
            k2_pickup_raw_[i] = (float)(sample_synth_imp_tune_semis_ + 24) / 48.f;
            break;
        }
        case KnobContext::SampleSynthImport: break; // no continuous knobs -- Button1/Button2 only
        case KnobContext::SampleSynthPreset: break; // browses a list directly, no pickup used
        case KnobContext::SampleSynthUser: break;   // ditto -- browses whichever face's own User-preset list
        case KnobContext::GranularGrainSizeFill:
            if(granular_)
            {
                k1_pickup_raw_[i] = granular_->GetSize01();
                k2_pickup_raw_[i] = granular_->GetFill01();
            }
            break;
        case KnobContext::GranularGrainGapJitter:
            if(granular_)
            {
                k1_pickup_raw_[i] = granular_->GetGap01();
                k2_pickup_raw_[i] = granular_->GetJitter01();
            }
            break;
        case KnobContext::GranularPosition:
            if(granular_)
            {
                k1_pickup_raw_[i] = granular_->GetPosition01();
                k2_pickup_raw_[i] = granular_->GetDirection01();
            }
            break;
        case KnobContext::GranularScanRange:
            if(granular_)
            {
                k1_pickup_raw_[i] = granular_->GetScanPosition01();
                k2_pickup_raw_[i] = granular_->GetScanEnd01();
            }
            break;
        case KnobContext::GranularScanGap:
            if(granular_)
                k1_pickup_raw_[i] = granular_->GetScanGap01();
            break;
        case KnobContext::GranularScan:
            if(granular_)
            {
                k1_pickup_raw_[i] = granular_->GetScan01();
                k2_pickup_raw_[i] = granular_->GetScanFill01();
            }
            break;
        case KnobContext::GranularTune:
            if(granular_)
                k1_pickup_raw_[i] = granular_->GetGrainTuneSemitones01();
            break;
        case KnobContext::GranularEnvAD:
            if(granular_)
            {
                k1_pickup_raw_[i] = granular_->GetAttack01();
                k2_pickup_raw_[i] = granular_->GetDecay01();
            }
            break;
        case KnobContext::GranularEnvSR:
            if(granular_)
            {
                k1_pickup_raw_[i] = granular_->GetSustain01();
                k2_pickup_raw_[i] = granular_->GetRelease01();
            }
            break;
        case KnobContext::GranularFilter:
            if(granular_)
            {
                k1_pickup_raw_[i] = granular_->GetFilterCutoff01();
                k2_pickup_raw_[i] = granular_->GetFilterResonance01();
            }
            break;
        case KnobContext::GranularMix:
            if(granular_)
            {
                k1_pickup_raw_[i] = granular_->GetGrainVolume01();
                k2_pickup_raw_[i] = granular_->GetScanVolume01();
            }
            break;
        case KnobContext::GranularMixReverb:
            if(granular_)
                k1_pickup_raw_[i] = granular_->GetReverbSend01();
            break;
        case KnobContext::GranularFxReverb:
            if(granular_)
            {
                k1_pickup_raw_[i] = granular_->GetReverbSend01();
                k2_pickup_raw_[i] = reverb_size01_;
            }
            break;
        case KnobContext::GranularFxDelay:
            if(granular_)
            {
                k1_pickup_raw_[i] = granular_->GetDelaySend01();
                k2_pickup_raw_[i] = delay_time01_;
            }
            break;
        case KnobContext::GranularMidi:
            // Same bucket-midpoint idiom as KnobContext::DexedMidi above.
            k1_pickup_raw_[i]
                = Clampf(((float)(granular_midi_channel_ + 1) + 0.5f) / 16.99f, 0.f, 1.f);
            break;
        case KnobContext::GranularCapture: break; // no continuous knobs, Button1/Button2 only
        case KnobContext::GranularTrim:
            k1_pickup_raw_[i] = granular_trim_start01_;
            k2_pickup_raw_[i] = granular_trim_end01_;
            break;
        case KnobContext::GranularPreset: break; // browses a list directly, no pickup used
        case KnobContext::MixerVolPan:
            k1_pickup_raw_[i] = MixerGetVolume01(mixer_position_);
            k2_pickup_raw_[i] = MixerGetPan01(mixer_position_);
            break;
        case KnobContext::MixerReverb:
            k1_pickup_raw_[i] = MixerGetSend01(mixer_position_);
            break;
        case KnobContext::MixerMaster:
            k1_pickup_raw_[i] = master_volume01_;
            k2_pickup_raw_[i] = reverb_size01_;
            break;
        case KnobContext::MixerNoKnobs: break; // Scope stop -- no continuous knobs
        default: break;
    }
}

bool Ui::KnobPickUp(float raw, float& stored_raw, bool& engaged)
{
    constexpr float kKnobPickupEpsilon = 0.04f;
    if(!engaged)
    {
        if(fabsf(raw - stored_raw) > kKnobPickupEpsilon)
            return false;
        engaged = true;
    }
    stored_raw = raw;
    return true;
}

void Ui::SetProjectSpeed01(float v)
{
    project_speed01_ = Clampf(v, 0.f, 1.f);
    project_speed_   = SpeedCurve01(project_speed01_);
}

void Ui::ScrubBy(int32_t inc)
{
    // ~50ms @48kHz per encoder detent at rest -- tune by feel.
    constexpr float kScrubSamplesPerClick = 2400.f;

    // Turn-speed acceleration: ticks arriving close together (a fast
    // spin) scrub much further per click than slow, deliberate ones --
    // otherwise covering real distance means a lot of turning. 150ms
    // between ticks or slower stays at the base 1x/~50ms-per-click feel;
    // under ~19ms between ticks (fast spin) ramps up to the 8x cap
    // (~400ms per click). Both constants are starting guesses, tune by
    // feel on real hardware.
    uint32_t now  = System::GetNow();
    uint32_t dt   = now - last_scrub_tick_ms_;
    last_scrub_tick_ms_ = now;
    float accel = dt > 0 ? Clampf(150.f / (float)dt, 1.f, 8.f) : 8.f;

    float delta = (float)inc * kScrubSamplesPerClick * accel;
    int   rep   = -1; // lowest-indexed non-empty layer, same one DrawSpeedScreen()
                       // reads for the playhead -- also used to re-sync
                       // TempoClock's phase below, since without that the
                       // metronome/beat indicator/bar-start would just keep
                       // free-running from wherever they already were,
                       // completely disconnected from where scrub moved
                       // the audio to (see TempoClock::SetPhaseToPosition()).
    for(int i = 0; i < num_layers_; i++)
    {
        if(!layers_[i].HasContent())
            continue;
        if(rep < 0)
            rep = i;
        float len = (float)layers_[i].GetRecordedLength();
        float pos = layers_[i].GetPlayPosRaw() + delta;
        while(pos >= len) pos -= len;
        while(pos < 0.f) pos += len;
        layers_[i].SetPlayPosRaw(pos);
        if(i == rep)
            tempo_->SetPhaseToPosition(pos);
    }
}

void Ui::NudgeFreezeBy(int32_t inc)
{
    // Same base distance/acceleration feel as ScrubBy() -- shares
    // last_scrub_tick_ms_ since Scrub and Freeze are mutually exclusive
    // (only one mode is ever active on Global:Speed at a time).
    constexpr float kScrubSamplesPerClick = 2400.f;

    uint32_t now  = System::GetNow();
    uint32_t dt   = now - last_scrub_tick_ms_;
    last_scrub_tick_ms_ = now;
    float accel = dt > 0 ? Clampf(150.f / (float)dt, 1.f, 8.f) : 8.f;

    float delta = (float)inc * kScrubSamplesPerClick * accel;
    for(int i = 0; i < num_layers_; i++)
    {
        if(!layers_[i].HasContent())
            continue;
        layers_[i].NudgeFreeze(delta);
    }
}

void Ui::TogglePauseAll()
{
    loop_paused_ = !loop_paused_;
    for(int i = 0; i < num_layers_; i++)
        layers_[i].SetPaused(loop_paused_);
}

void Ui::ApplyKnobs()
{
    // Deadband the raw pot reading itself, once, before it fans out to
    // every KnobContext below -- see k1_committed_/k2_committed_'s
    // comment in ui.h for why AnalogControl's own filter doesn't already
    // cover this. Small enough to be inaudible/invisible as a step (a
    // fraction of the 0.02 pickup epsilon) while absorbing the pot's
    // resting noise so parameters stop wobbling when the knob isn't
    // actually being turned.
    constexpr float kPotDriftDeadband = 0.004f;
    float            raw_k1           = pod_->GetKnobValue(DaisyPod::KNOB_1);
    float            raw_k2           = pod_->GetKnobValue(DaisyPod::KNOB_2);
    if(fabsf(raw_k1 - k1_committed_) > kPotDriftDeadband)
        k1_committed_ = raw_k1;
    if(fabsf(raw_k2 - k2_committed_) > kPotDriftDeadband)
        k2_committed_ = raw_k2;
    float k1 = k1_committed_;
    float k2 = k2_committed_;

    // Re-arm pickup on every context change -- see the KnobPickUp()
    // comment in ui.h for why this doesn't force an unnecessary wiggle
    // when the knob genuinely didn't move. Also re-seed the pickup
    // target to this context's actual current value (SyncPickupTargets)
    // so the knob regains control as soon as it crosses the real value,
    // not wherever it was last left for a different layer sharing this
    // same context (or 0 if this context has never been engaged yet).
    KnobContext ctx = CurrentKnobContext();
    if(ctx != last_knob_context_)
    {
        size_t i              = (size_t)ctx;
        k1_pickup_engaged_[i] = false;
        k2_pickup_engaged_[i] = false;
        SyncPickupTargets(ctx);
        last_knob_context_    = ctx;
    }
    size_t ci = (size_t)ctx;

    switch(screen_)
    {
        case Screen::Home:
            if(KnobPickUp(k1, k1_pickup_raw_[ci], k1_pickup_engaged_[ci]))
            {
                // Same 0..1.43x curve the original firmware used for
                // master volume, so overall loudness feels consistent.
                master_volume01_ = Clampf(k1, 0.f, 1.f);
                master_volume_   = powf(master_volume01_, 2.5f) * 1.43f;
                if(master_volume_ < 0.f)
                    master_volume_ = 0.f;
            }
            if(KnobPickUp(k2, k2_pickup_raw_[ci], k2_pickup_engaged_[ci]))
                tempo_->SetMetronomeVolume01(k2);
            break;

        case Screen::Layer:
            switch(layer_page_)
            {
                case LayerPage::Status:
                    if(KnobPickUp(k1, k1_pickup_raw_[ci], k1_pickup_engaged_[ci]))
                        Cur().SetVolume01(k1);
                    if(KnobPickUp(k2, k2_pickup_raw_[ci], k2_pickup_engaged_[ci]))
                        Cur().SetPan01(k2);
                    break;
                case LayerPage::Speed:
                    if(KnobPickUp(k1, k1_pickup_raw_[ci], k1_pickup_engaged_[ci]))
                        Cur().SetSpeed01(k1);
                    break;
                case LayerPage::Filter:
                    if(KnobPickUp(k1, k1_pickup_raw_[ci], k1_pickup_engaged_[ci]))
                        Cur().SetFilterCutoff01(k1);
                    if(KnobPickUp(k2, k2_pickup_raw_[ci], k2_pickup_engaged_[ci]))
                        Cur().SetFilterResonance01(k2);
                    break;
                case LayerPage::Effect:
                    if(KnobPickUp(k1, k1_pickup_raw_[ci], k1_pickup_engaged_[ci]))
                        Cur().SetEffectParamA01(k1);
                    if(KnobPickUp(k2, k2_pickup_raw_[ci], k2_pickup_engaged_[ci]))
                        Cur().SetEffectParamB01(k2);
                    break;
                case LayerPage::Reverb:
                    // Send is per-layer for both effects -- Size/Time/
                    // Feedback are shared Global:FX settings (see the
                    // shared-bus comment on LooperLayer::SetReverbSend01()/
                    // SetDelaySend01()), so this page is just the two Send
                    // knobs, no toggle needed.
                    if(KnobPickUp(k1, k1_pickup_raw_[ci], k1_pickup_engaged_[ci]))
                        Cur().SetReverbSend01(k1);
                    if(KnobPickUp(k2, k2_pickup_raw_[ci], k2_pickup_engaged_[ci]))
                        Cur().SetDelaySend01(k2);
                    break;
                case LayerPage::Gain:
                    if(KnobPickUp(k1, k1_pickup_raw_[ci], k1_pickup_engaged_[ci]))
                        Cur().SetInputGain01(k1);
                    break;
                default: break;
            }
            break;

        case Screen::Global:
            if(global_page_ == GlobalPage::Tempo && !tempo_->IsLocked())
            {
                if(KnobPickUp(k1, k1_pickup_raw_[ci], k1_pickup_engaged_[ci]))
                    tempo_->SetBpm(50.f + k1 * (240.f - 50.f));
                if(KnobPickUp(k2, k2_pickup_raw_[ci], k2_pickup_engaged_[ci]))
                {
                    // Bars' own span is TempoClock::MaxBarsForBpm(bpm) --
                    // turning K2 fully clockwise always reaches whatever's
                    // actually allowed at the BPM just set above, not a
                    // flat 16. -0.01 keeps k2==1.0 landing on exactly
                    // max_bars (see SyncPickupTargets()'s matching inverse).
                    float max_bars_span
                        = (float)TempoClock::MaxBarsForBpm(tempo_->GetBpm()) - 0.01f;
                    tempo_->SetBars(1 + (int)(k2 * max_bars_span));
                }
            }
            else if(global_page_ == GlobalPage::Filter)
            {
                if(KnobPickUp(k1, k1_pickup_raw_[ci], k1_pickup_engaged_[ci]))
                    master_filter_cutoff01_ = Clampf(k1, 0.f, 1.f);
                if(KnobPickUp(k2, k2_pickup_raw_[ci], k2_pickup_engaged_[ci]))
                    master_filter_res01_ = Clampf(k2, 0.f, 1.f);
            }
            else if(global_page_ == GlobalPage::Reverb)
            {
                if(!global_fx_target_delay_)
                {
                    if(KnobPickUp(k1, k1_pickup_raw_[ci], k1_pickup_engaged_[ci]))
                        reverb_size01_ = Clampf(k1, 0.f, 1.f);
                    if(KnobPickUp(k2, k2_pickup_raw_[ci], k2_pickup_engaged_[ci]))
                        bypass_reverb_send01_ = Clampf(k2, 0.f, 1.f);
                }
                else
                {
                    if(KnobPickUp(k1, k1_pickup_raw_[ci], k1_pickup_engaged_[ci]))
                        delay_time01_ = Clampf(k1, 0.f, 1.f);
                    if(KnobPickUp(k2, k2_pickup_raw_[ci], k2_pickup_engaged_[ci]))
                        delay_feedback01_ = Clampf(k2, 0.f, 1.f);
                }
            }
            else if(global_page_ == GlobalPage::ReverbTone)
            {
                if(!global_reverb_tone_target_b_)
                {
                    if(KnobPickUp(k1, k1_pickup_raw_[ci], k1_pickup_engaged_[ci]))
                        reverb_predelay01_ = Clampf(k1, 0.f, 1.f);
                    if(KnobPickUp(k2, k2_pickup_raw_[ci], k2_pickup_engaged_[ci]))
                        reverb_prefilter01_ = Clampf(k2, 0.f, 1.f);
                }
                else
                {
                    if(KnobPickUp(k1, k1_pickup_raw_[ci], k1_pickup_engaged_[ci]))
                        reverb_decay_diffusion01_ = Clampf(k1, 0.f, 1.f);
                    if(KnobPickUp(k2, k2_pickup_raw_[ci], k2_pickup_engaged_[ci]))
                        reverb_damping01_ = Clampf(k2, 0.f, 1.f);
                }
            }
            else if(global_page_ == GlobalPage::ReverbDiffusion)
            {
                if(KnobPickUp(k1, k1_pickup_raw_[ci], k1_pickup_engaged_[ci]))
                    reverb_input_diffusion1_01_ = Clampf(k1, 0.f, 1.f);
                if(KnobPickUp(k2, k2_pickup_raw_[ci], k2_pickup_engaged_[ci]))
                    reverb_input_diffusion2_01_ = Clampf(k2, 0.f, 1.f);
            }
            else if(global_page_ == GlobalPage::Speed)
            {
                if(KnobPickUp(k1, k1_pickup_raw_[ci], k1_pickup_engaged_[ci]))
                    SetProjectSpeed01(k1);
            }
            else if(global_page_ == GlobalPage::File)
            {
                if(save_load_mode_ == SaveLoadMode::ChoosingSave)
                {
                    // Overwrite is only a real option once something's
                    // actually loaded -- otherwise force Save New so
                    // Knob1 can't land on a choice that doesn't exist.
                    save_as_new_ = loaded_slot_ < 0 ? true : k1 >= 0.5f;
                }
                else if(save_load_mode_ == SaveLoadMode::BrowsingLoad)
                {
                    if(!load_browsing_files_)
                    {
                        // Top-level chooser (mirrors ChoosingSave's own
                        // Overwrite/Save New pick above) -- forced to New
                        // when there's nothing to browse, same "can't
                        // select what doesn't exist" reasoning.
                        load_new_selected_ = file_slot_count_ == 0 ? true : k1 >= 0.5f;
                    }
                    else if(file_slot_count_ > 0)
                    {
                        // Drilled into the numbered list (Button1's own
                        // tap, see OnButton1Short()) -- discretized
                        // browse, not pickup-tracked, same idiom as
                        // every other list-browse knob in this project.
                        int idx = (int)(Clampf(k1, 0.f, 1.f) * file_slot_count_);
                        if(idx >= file_slot_count_)
                            idx = file_slot_count_ - 1;
                        file_cursor_ = idx;
                    }
                }
            }
            // (GlobalPage::Mixer has no knobs of its own any more -- entry
            // point only, same as GlobalPage::Granular; see Screen::Mixer
            // for the real editing surface. No branch needed for it here.)
            else if(global_page_ == GlobalPage::SdMgmt)
            {
                if(!sd_mgmt_in_folder_)
                {
                    // Knob1 picks which folder -- discretized among the 3
                    // categories, same idiom as every other list-browse
                    // knob in this project.
                    int n   = (int)SdMgmtFolder::kCount;
                    int idx = (int)(Clampf(k1, 0.f, 1.f) * n);
                    if(idx >= n)
                        idx = n - 1;
                    sd_mgmt_folder_ = (SdMgmtFolder)idx;
                }
                else
                {
                    int count = 0;
                    if(SdMgmtIsBankFolder())
                        count = SdMgmtBankLevelCount();
                    else
                        SdMgmtSlots(&count);
                    if(count > 0)
                    {
                        int idx = (int)(Clampf(k1, 0.f, 1.f) * count);
                        if(idx >= count)
                            idx = count - 1;
                        if(idx != sd_mgmt_cursor_)
                            sd_mgmt_status_[0] = '\0';
                        sd_mgmt_cursor_ = idx;
                    }
                }
            }
            break;

        case Screen::Granular:
            if(!granular_)
                break;
            switch(granular_param_page_)
            {
                case GranularParamPage::Grain:
                    if(!granular_grain_target_gap_jitter_)
                    {
                        if(KnobPickUp(k1, k1_pickup_raw_[ci], k1_pickup_engaged_[ci]))
                            granular_->SetSize01(k1);
                        if(KnobPickUp(k2, k2_pickup_raw_[ci], k2_pickup_engaged_[ci]))
                            granular_->SetFill01(k2);
                    }
                    else
                    {
                        if(KnobPickUp(k1, k1_pickup_raw_[ci], k1_pickup_engaged_[ci]))
                            granular_->SetGap01(k1);
                        if(KnobPickUp(k2, k2_pickup_raw_[ci], k2_pickup_engaged_[ci]))
                            granular_->SetJitter01(k2);
                    }
                    break;
                case GranularParamPage::Position:
                    if(KnobPickUp(k1, k1_pickup_raw_[ci], k1_pickup_engaged_[ci]))
                        granular_->SetPosition01(k1);
                    if(KnobPickUp(k2, k2_pickup_raw_[ci], k2_pickup_engaged_[ci]))
                        granular_->SetDirection01(k2);
                    break;
                case GranularParamPage::ScanRange:
                    if(!granular_scanrange_target_gap_)
                    {
                        if(KnobPickUp(k1, k1_pickup_raw_[ci], k1_pickup_engaged_[ci]))
                            granular_->SetScanPosition01(k1);
                        if(KnobPickUp(k2, k2_pickup_raw_[ci], k2_pickup_engaged_[ci]))
                            granular_->SetScanEnd01(k2);
                    }
                    else
                    {
                        if(KnobPickUp(k1, k1_pickup_raw_[ci], k1_pickup_engaged_[ci]))
                            granular_->SetScanGap01(k1);
                    }
                    break;
                case GranularParamPage::Scan:
                    if(KnobPickUp(k1, k1_pickup_raw_[ci], k1_pickup_engaged_[ci]))
                        granular_->SetScan01(k1);
                    if(KnobPickUp(k2, k2_pickup_raw_[ci], k2_pickup_engaged_[ci]))
                        granular_->SetScanFill01(k2);
                    break;
                case GranularParamPage::Tune:
                    if(KnobPickUp(k1, k1_pickup_raw_[ci], k1_pickup_engaged_[ci]))
                        granular_->SetGrainTuneSemitones01(k1);
                    break;
                case GranularParamPage::ADSR:
                    if(!granular_adsr_target_sr_)
                    {
                        if(KnobPickUp(k1, k1_pickup_raw_[ci], k1_pickup_engaged_[ci]))
                            granular_->SetAttack01(k1);
                        if(KnobPickUp(k2, k2_pickup_raw_[ci], k2_pickup_engaged_[ci]))
                            granular_->SetDecay01(k2);
                    }
                    else
                    {
                        if(KnobPickUp(k1, k1_pickup_raw_[ci], k1_pickup_engaged_[ci]))
                            granular_->SetSustain01(k1);
                        if(KnobPickUp(k2, k2_pickup_raw_[ci], k2_pickup_engaged_[ci]))
                            granular_->SetRelease01(k2);
                    }
                    break;
                case GranularParamPage::Filter:
                    if(KnobPickUp(k1, k1_pickup_raw_[ci], k1_pickup_engaged_[ci]))
                        granular_->SetFilterCutoff01(k1);
                    if(KnobPickUp(k2, k2_pickup_raw_[ci], k2_pickup_engaged_[ci]))
                        granular_->SetFilterResonance01(k2);
                    break;
                case GranularParamPage::Mix:
                    if(!granular_mix_target_reverb_)
                    {
                        if(KnobPickUp(k1, k1_pickup_raw_[ci], k1_pickup_engaged_[ci]))
                            granular_->SetGrainVolume01(k1);
                        if(KnobPickUp(k2, k2_pickup_raw_[ci], k2_pickup_engaged_[ci]))
                            granular_->SetScanVolume01(k2);
                    }
                    else
                    {
                        if(KnobPickUp(k1, k1_pickup_raw_[ci], k1_pickup_engaged_[ci]))
                            granular_->SetReverbSend01(k1);
                    }
                    break;
                case GranularParamPage::FX:
                    if(!granular_fx_target_delay_)
                    {
                        if(KnobPickUp(k1, k1_pickup_raw_[ci], k1_pickup_engaged_[ci]))
                            granular_->SetReverbSend01(k1);
                        if(KnobPickUp(k2, k2_pickup_raw_[ci], k2_pickup_engaged_[ci]))
                            reverb_size01_ = Clampf(k2, 0.f, 1.f);
                    }
                    else
                    {
                        if(KnobPickUp(k1, k1_pickup_raw_[ci], k1_pickup_engaged_[ci]))
                            granular_->SetDelaySend01(k1);
                        if(KnobPickUp(k2, k2_pickup_raw_[ci], k2_pickup_engaged_[ci]))
                            delay_time01_ = Clampf(k2, 0.f, 1.f);
                    }
                    break;
                case GranularParamPage::MIDI:
                    if(KnobPickUp(k1, k1_pickup_raw_[ci], k1_pickup_engaged_[ci]))
                        granular_midi_channel_ = -1 + (int)(k1 * 16.99f);
                    break;
                case GranularParamPage::Capture:
                {
                    // Only meaningful in Import mode -- Direct Record and
                    // From Layer have no continuous knob use here. Same
                    // discretized "browse a list directly" idiom as
                    // Preset's own knob below, no pickup tracking.
                    if(granular_capture_source_ == -2)
                    {
                        int total = granular_import_file_count_;
                        if(total > 0)
                        {
                            int idx = (int)(Clampf(k1, 0.f, 1.f) * total);
                            if(idx >= total)
                                idx = total - 1;
                            if(idx != granular_import_cursor_)
                                granular_capture_status_[0] = '\0';
                            granular_import_cursor_ = idx;
                        }
                    }
                    break;
                }
                case GranularParamPage::Trim:
                {
                    bool changed = false;
                    if(KnobPickUp(k1, k1_pickup_raw_[ci], k1_pickup_engaged_[ci]))
                    {
                        granular_trim_start01_ = k1;
                        changed                = true;
                    }
                    if(KnobPickUp(k2, k2_pickup_raw_[ci], k2_pickup_engaged_[ci]))
                    {
                        granular_trim_end01_ = k2;
                        changed              = true;
                    }
                    if(changed)
                        ApplyGranularTrim();
                    break;
                }
                case GranularParamPage::Preset:
                {
                    if(save_load_mode_ == SaveLoadMode::ChoosingSave)
                    {
                        save_as_new_ = granular_loaded_preset_slot_ <= 0 ? true : k1 >= 0.5f;
                        break;
                    }
                    if(save_load_mode_ != SaveLoadMode::BrowsingLoad)
                        break;
                    if(!load_browsing_files_)
                    {
                        // Top-level chooser (mirrors ChoosingSave's own
                        // Overwrite/Save New pick above) -- forced to New
                        // when there's nothing to browse, same "can't
                        // select what doesn't exist" reasoning.
                        load_new_selected_
                            = granular_preset_user_slot_count_ == 0 ? true : k1 >= 0.5f;
                        break;
                    }
                    // Drilled into the numbered list (Button1's own tap,
                    // see OnButton1Short()) -- discretized browse, not
                    // pickup-tracked, same idiom as Global:File's own
                    // file_cursor_ (no factory range here to fold in).
                    if(granular_preset_user_slot_count_ > 0)
                    {
                        int total = granular_preset_user_slot_count_;
                        int idx   = (int)(Clampf(k1, 0.f, 1.f) * total);
                        if(idx >= total)
                            idx = total - 1;
                        if(idx != granular_preset_cursor_)
                            granular_preset_status_[0] = '\0';
                        granular_preset_cursor_ = idx;
                    }
                    break;
                }
                default: break;
            }
            break;

        case Screen::Dexed:
            if(!dexed_)
                break;
            switch(dexed_param_page_)
            {
                // Every case below that touches dexed_'s own
                // SetPatchByte()/SetBrightness01()/SetEnvSpeed01() gates
                // on a real "did this actually change" check before
                // calling it -- unlike every other engine's own knob-
                // driven setter in this project (a cheap float
                // assignment KnobPickUp()'s own lack of a change-check
                // was always harmless to call every tick against), these
                // trigger a real per-held-voice Dx7Note::update() call
                // (see ApplyPatchToHeldVoices()) inside a brief critical
                // section (see NoteOn()'s own comment on why that's
                // needed at all). Calling that every single main-loop
                // tick the knob is engaged -- including while sitting
                // still, since KnobPickUp() itself never checks for
                // real movement -- was enough cumulative critical-
                // section time with several notes held to audibly drop
                // notes while turning the knob, confirmed on real
                // hardware.
                case DexedParamPage::Algo:
                    if(KnobPickUp(k1, k1_pickup_raw_[ci], k1_pickup_engaged_[ci]))
                    {
                        int algo = (int)(Clampf(k1, 0.f, 1.f) * 32.f);
                        algo     = algo > 31 ? 31 : algo;
                        if((uint8_t)algo != dexed_->GetPatchByte(134))
                            dexed_->SetPatchByte(134, (uint8_t)algo);
                    }
                    break;
                case DexedParamPage::Feedback:
                    if(KnobPickUp(k1, k1_pickup_raw_[ci], k1_pickup_engaged_[ci]))
                    {
                        int fb = (int)(Clampf(k1, 0.f, 1.f) * 7.f + 0.5f);
                        fb     = fb > 7 ? 7 : fb;
                        if((uint8_t)fb != dexed_->GetPatchByte(135))
                            dexed_->SetPatchByte(135, (uint8_t)fb);
                    }
                    break;
                case DexedParamPage::Vibrato:
                    if(!dexed_vibrato_target_sens_)
                    {
                        if(KnobPickUp(k1, k1_pickup_raw_[ci], k1_pickup_engaged_[ci]))
                        {
                            int speed = (int)(Clampf(k1, 0.f, 1.f) * 99.f + 0.5f);
                            speed     = speed > 99 ? 99 : speed;
                            if((uint8_t)speed != dexed_->GetPatchByte(137))
                                dexed_->SetPatchByte(137, (uint8_t)speed);
                        }
                        if(KnobPickUp(k2, k2_pickup_raw_[ci], k2_pickup_engaged_[ci]))
                        {
                            int depth = (int)(Clampf(k2, 0.f, 1.f) * 99.f + 0.5f);
                            depth     = depth > 99 ? 99 : depth;
                            if((uint8_t)depth != dexed_->GetPatchByte(139))
                                dexed_->SetPatchByte(139, (uint8_t)depth);
                        }
                    }
                    else if(KnobPickUp(k1, k1_pickup_raw_[ci], k1_pickup_engaged_[ci]))
                    {
                        // Global Pitch Mod Sensitivity -- only bits 0-2 of
                        // this byte are ever read (patch[143] & 7, see
                        // msfa's own pitchmodsenstab lookup), so only
                        // those 8 values are worth quantizing to.
                        int sens = (int)(Clampf(k1, 0.f, 1.f) * 7.f + 0.5f);
                        sens     = sens > 7 ? 7 : sens;
                        if((uint8_t)sens != (dexed_->GetPatchByte(143) & 7))
                            dexed_->SetPatchByte(143, (uint8_t)sens);
                    }
                    break;
                case DexedParamPage::Brightness:
                    if(KnobPickUp(k1, k1_pickup_raw_[ci], k1_pickup_engaged_[ci])
                       && fabsf(k1 - dexed_->GetBrightness01()) > 0.002f)
                        dexed_->SetBrightness01(k1);
                    break;
                case DexedParamPage::EnvSpeed:
                    if(KnobPickUp(k1, k1_pickup_raw_[ci], k1_pickup_engaged_[ci])
                       && fabsf(k1 - dexed_->GetEnvSpeed01()) > 0.002f)
                        dexed_->SetEnvSpeed01(k1);
                    break;
                case DexedParamPage::Filter:
                    if(KnobPickUp(k1, k1_pickup_raw_[ci], k1_pickup_engaged_[ci]))
                        dexed_->SetFilterCutoff01(k1);
                    if(KnobPickUp(k2, k2_pickup_raw_[ci], k2_pickup_engaged_[ci]))
                        dexed_->SetFilterResonance01(k2);
                    break;
                case DexedParamPage::Mix:
                    if(KnobPickUp(k1, k1_pickup_raw_[ci], k1_pickup_engaged_[ci]))
                        dexed_->SetReverbSend01(k1);
                    if(KnobPickUp(k2, k2_pickup_raw_[ci], k2_pickup_engaged_[ci]))
                        dexed_->SetOutputLevel01(k2);
                    break;
                case DexedParamPage::FX:
                    if(!dexed_fx_target_delay_)
                    {
                        if(KnobPickUp(k1, k1_pickup_raw_[ci], k1_pickup_engaged_[ci]))
                            dexed_->SetReverbSend01(k1);
                        if(KnobPickUp(k2, k2_pickup_raw_[ci], k2_pickup_engaged_[ci]))
                            reverb_size01_ = Clampf(k2, 0.f, 1.f);
                    }
                    else
                    {
                        if(KnobPickUp(k1, k1_pickup_raw_[ci], k1_pickup_engaged_[ci]))
                            dexed_->SetDelaySend01(k1);
                        if(KnobPickUp(k2, k2_pickup_raw_[ci], k2_pickup_engaged_[ci]))
                            delay_time01_ = Clampf(k2, 0.f, 1.f);
                    }
                    break;
                case DexedParamPage::Fold:
                    // K1 = Fold Amount, K2 = output trim -- no
                    // sub-mode, unlike FX above.
                    if(KnobPickUp(k1, k1_pickup_raw_[ci], k1_pickup_engaged_[ci]))
                        dexed_->SetFoldAmount01(k1);
                    if(KnobPickUp(k2, k2_pickup_raw_[ci], k2_pickup_engaged_[ci]))
                        dexed_->SetFoldTrim01(k2);
                    break;
                case DexedParamPage::MIDI:
                    if(KnobPickUp(k1, k1_pickup_raw_[ci], k1_pickup_engaged_[ci]))
                        dexed_midi_channel_ = -1 + (int)(k1 * 16.99f);
                    break;
                case DexedParamPage::Advanced: break; // no knobs, encoder click drills in
                case DexedParamPage::Preset:
                {
                    if(save_load_mode_ == SaveLoadMode::ChoosingSave)
                    {
                        // Overwrite is only a real option once a real
                        // user slot (not a factory one) is loaded.
                        save_as_new_
                            = dexed_loaded_preset_slot_ <= DexedSynth::GetNumFactoryPresets()
                                  ? true
                                  : k1 >= 0.5f;
                        break;
                    }
                    if(save_load_mode_ != SaveLoadMode::BrowsingLoad)
                        break;
                    if(dexed_import_browsing_)
                    {
                        // Flat .syx file list (DXIMPORT/) -- same
                        // discretized "browse a list directly" idiom as
                        // the folder-list/in-folder cases below, just one
                        // level instead of two since there's no factory-
                        // category structure for an arbitrary SD folder.
                        int total = dexed_import_file_count_;
                        if(total > 0)
                        {
                            int idx = (int)(Clampf(k1, 0.f, 1.f) * total);
                            if(idx >= total)
                                idx = total - 1;
                            if(idx != dexed_import_cursor_)
                                dexed_preset_status_[0] = '\0';
                            dexed_import_cursor_ = idx;
                        }
                        break;
                    }
                    if(!load_browsing_files_)
                    {
                        // Top-level chooser (mirrors ChoosingSave's own
                        // Overwrite/Save New pick above), now 3-way:
                        // Files (factory presets always exist, so 0..1/3
                        // is always a real option), Import (SD SysEx),
                        // New.
                        float k1c               = Clampf(k1, 0.f, 1.f);
                        load_new_selected_       = k1c >= (2.f / 3.f);
                        dexed_import_selected_   = !load_new_selected_ && k1c >= (1.f / 3.f);
                        break;
                    }
                    if(!dexed_files_group_open_)
                    {
                        // Group chooser (see DexedFilesGroup's own
                        // comment) -- Roms/Dexed/Imports/User.
                        int total = (int)DexedFilesGroup::kCount;
                        int idx   = (int)(Clampf(k1, 0.f, 1.f) * total);
                        if(idx >= total)
                            idx = total - 1;
                        // Real bug, confirmed on hardware: marking
                        // dexed_preset_slots_dirty_ here (this group
                        // chooser's own display doesn't even show
                        // folder counts, only a static Roms/Dexed/
                        // Imports/User label list) forced a real SD
                        // directory scan on every single group the knob
                        // swept past, not just the one it settled on --
                        // visibly laggy scrubbing through all 4 quickly.
                        // The real refresh already happens on actually
                        // opening a group (see HandleButton2()'s own
                        // "Open" handling), which is the only point this
                        // group's own preset/folder counts are needed.
                        dexed_files_group_ = (DexedFilesGroup)idx;
                        break;
                    }
                    // User has no folder level of its own (see
                    // DexedFilesGroup's own comment) -- only
                    // Roms/Dexed/Imports get this extra level, K1
                    // scrolling dexed_preset_folder_cursor_ across
                    // whichever this group's own folder list holds.
                    if(dexed_files_group_ != DexedFilesGroup::User && !dexed_preset_folder_open_)
                    {
                        int total;
                        switch(dexed_files_group_)
                        {
                            case DexedFilesGroup::Roms: total = DexedSynth::kNumRomCategories; break;
                            case DexedFilesGroup::Dexed:
                                total = DexedSynth::kNumFactoryCategories
                                        - DexedSynth::kNumRomCategories;
                                break;
                            case DexedFilesGroup::Imports:
                            default: total = dexed_import_folder_count_; break;
                        }
                        int idx = total > 0 ? (int)(Clampf(k1, 0.f, 1.f) * total) : 0;
                        if(idx >= total)
                            idx = total > 0 ? total - 1 : 0;
                        dexed_preset_folder_cursor_ = idx;
                        break;
                    }
                    {
                        // Deepest level -- either inside a Roms/Dexed
                        // factory folder, inside one Imports bank's own
                        // slot range, or (User) the flat, import-filtered
                        // slot list directly.
                        int total;
                        if(dexed_files_group_ == DexedFilesGroup::User)
                            total = dexed_preset_user_slot_count_;
                        else if(dexed_files_group_ == DexedFilesGroup::Imports)
                        {
                            total = (dexed_preset_folder_cursor_ >= 0
                                     && dexed_preset_folder_cursor_ < dexed_import_folder_count_)
                                        ? dexed_import_folders_[dexed_preset_folder_cursor_].last_slot
                                              - dexed_import_folders_[dexed_preset_folder_cursor_]
                                                    .first_slot
                                              + 1
                                        : 0;
                        }
                        else
                            total = DexedSynth::GetFactoryCategoryCount(ResolveDexedRealCategory());
                        if(total <= 0)
                            break;
                        int idx = (int)(Clampf(k1, 0.f, 1.f) * total);
                        if(idx >= total)
                            idx = total - 1;
                        // Browsing again -- clear the last save/load
                        // result so the "Load:" line (which shows what
                        // the knob is actually pointing at right now)
                        // comes back instead of staying stuck on a
                        // status message that never otherwise clears.
                        if(idx != dexed_preset_cursor_)
                            dexed_preset_status_[0] = '\0';
                        dexed_preset_cursor_ = idx;
                    }
                    break;
                }
                default: break;
            }
            break;

        case Screen::Ensemble:
            if(!ensemble_)
                break;
            switch(ensemble_param_page_)
            {
                case EnsembleParamPage::Registration:
                {
                    // K1 browses which of the 7 drawbars is selected --
                    // same "no pickup, direct scrub" idiom as Preset
                    // browsing/DexedFilesGroup elsewhere (this is a
                    // selector, not a persisted value of its own).
                    int idx = (int)(Clampf(k1, 0.f, 1.f) * EnsembleSynth::kNumDrawbars);
                    if(idx >= EnsembleSynth::kNumDrawbars)
                        idx = EnsembleSynth::kNumDrawbars - 1;
                    if(idx != ensemble_drawbar_index_)
                    {
                        ensemble_drawbar_index_ = idx;
                        // K2 now edits a DIFFERENT drawbar's level --
                        // re-arm its own pickup and reseed it to that
                        // drawbar's real current value, same "same
                        // KnobContext, manual resync" fix as
                        // dexed_op_index_/mixer_position_ use elsewhere.
                        k2_pickup_engaged_[ci] = false;
                        if(ensemble_)
                            k2_pickup_raw_[ci] = ensemble_->GetDrawbarLevel(ensemble_drawbar_index_);
                    }
                    if(ensemble_ && KnobPickUp(k2, k2_pickup_raw_[ci], k2_pickup_engaged_[ci]))
                        ensemble_->SetDrawbarLevel(ensemble_drawbar_index_, k2);
                    break;
                }
                case EnsembleParamPage::Env:
                    if(!ensemble_env_target_sr_)
                    {
                        if(KnobPickUp(k1, k1_pickup_raw_[ci], k1_pickup_engaged_[ci]))
                            ensemble_->SetAttack01(k1);
                        if(KnobPickUp(k2, k2_pickup_raw_[ci], k2_pickup_engaged_[ci]))
                            ensemble_->SetDecay01(k2);
                    }
                    else
                    {
                        if(KnobPickUp(k1, k1_pickup_raw_[ci], k1_pickup_engaged_[ci]))
                            ensemble_->SetSustain01(k1);
                        if(KnobPickUp(k2, k2_pickup_raw_[ci], k2_pickup_engaged_[ci]))
                            ensemble_->SetRelease01(k2);
                    }
                    break;
                case EnsembleParamPage::Ensemble:
                    if(KnobPickUp(k1, k1_pickup_raw_[ci], k1_pickup_engaged_[ci]))
                        ensemble_->SetChorusDepth01(k1);
                    if(KnobPickUp(k2, k2_pickup_raw_[ci], k2_pickup_engaged_[ci]))
                        ensemble_->SetChorusRate01(k2);
                    break;
                case EnsembleParamPage::Filter:
                    if(KnobPickUp(k1, k1_pickup_raw_[ci], k1_pickup_engaged_[ci]))
                        ensemble_->SetFilterCutoff01(k1);
                    if(KnobPickUp(k2, k2_pickup_raw_[ci], k2_pickup_engaged_[ci]))
                        ensemble_->SetFilterResonance01(k2);
                    break;
                case EnsembleParamPage::Mix:
                    if(KnobPickUp(k1, k1_pickup_raw_[ci], k1_pickup_engaged_[ci]))
                        ensemble_->SetOutputLevel01(k1);
                    if(KnobPickUp(k2, k2_pickup_raw_[ci], k2_pickup_engaged_[ci]))
                        ensemble_->SetPan01(k2);
                    break;
                case EnsembleParamPage::FX:
                    if(!ensemble_fx_target_delay_)
                    {
                        if(KnobPickUp(k1, k1_pickup_raw_[ci], k1_pickup_engaged_[ci]))
                            ensemble_->SetReverbSend01(k1);
                        if(KnobPickUp(k2, k2_pickup_raw_[ci], k2_pickup_engaged_[ci]))
                            reverb_size01_ = Clampf(k2, 0.f, 1.f);
                    }
                    else
                    {
                        if(KnobPickUp(k1, k1_pickup_raw_[ci], k1_pickup_engaged_[ci]))
                            ensemble_->SetDelaySend01(k1);
                        if(KnobPickUp(k2, k2_pickup_raw_[ci], k2_pickup_engaged_[ci]))
                            delay_time01_ = Clampf(k2, 0.f, 1.f);
                    }
                    break;
                case EnsembleParamPage::MIDI:
                    if(KnobPickUp(k1, k1_pickup_raw_[ci], k1_pickup_engaged_[ci]))
                        ensemble_midi_channel_ = -1 + (int)(k1 * 16.99f);
                    break;
                case EnsembleParamPage::Preset:
                {
                    if(save_load_mode_ == SaveLoadMode::ChoosingSave)
                    {
                        // Overwrite is only a real option once a real
                        // user slot (not a factory one) is loaded -- same
                        // idiom as DexedParamPage::Preset above.
                        save_as_new_
                            = ensemble_loaded_preset_slot_ <= EnsembleSynth::GetNumFactoryPresets()
                                  ? true
                                  : k1 >= 0.5f;
                        break;
                    }
                    if(save_load_mode_ != SaveLoadMode::BrowsingLoad)
                        break;
                    if(!load_browsing_files_)
                    {
                        // Top-level chooser -- Files is always a real
                        // option here (the factory bank always exists,
                        // unlike Grains' own Preset page), same reasoning
                        // as Dexed's own top-level chooser.
                        load_new_selected_ = k1 >= 0.5f;
                        break;
                    }
                    // Drilled into the combined [factory][user] list --
                    // see ensemble_preset_user_slots_'s own comment.
                    {
                        int total = EnsembleSynth::GetNumFactoryPresets()
                                    + ensemble_preset_user_slot_count_;
                        if(total > 0)
                        {
                            int idx = (int)(Clampf(k1, 0.f, 1.f) * total);
                            if(idx >= total)
                                idx = total - 1;
                            if(idx != ensemble_preset_cursor_)
                                ensemble_preset_status_[0] = '\0';
                            ensemble_preset_cursor_ = idx;
                        }
                    }
                    break;
                }
                default: break;
            }
            break;

        case Screen::SampleSynth:
        {
            if(!sample_synth_)
                break;
            switch(sample_synth_param_page_)
            {
                case SampleSynthParamPage::Filter:
                    if(KnobPickUp(k1, k1_pickup_raw_[ci], k1_pickup_engaged_[ci]))
                        sample_synth_->SetFilterCutoff01(k1);
                    if(KnobPickUp(k2, k2_pickup_raw_[ci], k2_pickup_engaged_[ci]))
                        sample_synth_->SetFilterResonance01(k2);
                    break;
                case SampleSynthParamPage::Env:
                    if(!sample_synth_adsr_target_sr_)
                    {
                        if(KnobPickUp(k1, k1_pickup_raw_[ci], k1_pickup_engaged_[ci]))
                            sample_synth_->SetAttack01(k1);
                        if(KnobPickUp(k2, k2_pickup_raw_[ci], k2_pickup_engaged_[ci]))
                            sample_synth_->SetDecay01(k2);
                    }
                    else
                    {
                        if(KnobPickUp(k1, k1_pickup_raw_[ci], k1_pickup_engaged_[ci]))
                            sample_synth_->SetSustain01(k1);
                        if(KnobPickUp(k2, k2_pickup_raw_[ci], k2_pickup_engaged_[ci]))
                            sample_synth_->SetRelease01(k2);
                    }
                    break;
                case SampleSynthParamPage::Mix:
                    if(KnobPickUp(k1, k1_pickup_raw_[ci], k1_pickup_engaged_[ci]))
                        sample_synth_->SetOutputLevel01(k1);
                    if(KnobPickUp(k2, k2_pickup_raw_[ci], k2_pickup_engaged_[ci]))
                        sample_synth_->SetPan01(k2);
                    break;
                case SampleSynthParamPage::Setup:
                    if(KnobPickUp(k1, k1_pickup_raw_[ci], k1_pickup_engaged_[ci]))
                    {
                        // Same +-24 semitone range/quantization as
                        // SampleSynthParamPage::Tune's own case above --
                        // applied live to every active slot, see
                        // SampleSynth::SetSlotPitchOffsetSemis()'s own
                        // comment for why this works for chromatic and
                        // one-shot slots alike.
                        float v01   = Clampf(k1, 0.f, 1.f);
                        int   semis = (int)(v01 * 48.f + 0.5f) - 24; // -24..+24
                        sample_synth_setup_tune_semis_ = (int8_t)semis;
                        for(int si = 0; si < SampleSynth::kMaxSampleSlots; si++)
                            if(sample_synth_->SlotHasSample(si))
                                sample_synth_->SetSlotPitchOffsetSemis(si, (int8_t)semis);
                    }
                    if(KnobPickUp(k2, k2_pickup_raw_[ci], k2_pickup_engaged_[ci]))
                    {
                        // Pending only -- Button2 held confirms it (see
                        // ApplySampleSynthBreakDivide()'s own comment).
                        float v01 = Clampf(k2, 0.f, 1.f);
                        sample_synth_setup_pending_slices_
                            = (int)(v01 * 16.f + 0.5f); // 0-16
                    }
                    break;
                case SampleSynthParamPage::FX:
                    if(!sample_synth_fx_target_delay_)
                    {
                        if(KnobPickUp(k1, k1_pickup_raw_[ci], k1_pickup_engaged_[ci]))
                            sample_synth_->SetReverbSend01(k1);
                        if(KnobPickUp(k2, k2_pickup_raw_[ci], k2_pickup_engaged_[ci]))
                            reverb_size01_ = Clampf(k2, 0.f, 1.f);
                    }
                    else
                    {
                        if(KnobPickUp(k1, k1_pickup_raw_[ci], k1_pickup_engaged_[ci]))
                            sample_synth_->SetDelaySend01(k1);
                        if(KnobPickUp(k2, k2_pickup_raw_[ci], k2_pickup_engaged_[ci]))
                            delay_time01_ = Clampf(k2, 0.f, 1.f);
                    }
                    break;
                case SampleSynthParamPage::MIDI:
                    if(KnobPickUp(k1, k1_pickup_raw_[ci], k1_pickup_engaged_[ci]))
                        sample_synth_midi_channel_ = -1 + (int)(k1 * 16.99f);
                    break;
                case SampleSynthParamPage::Capture:
                {
                    // Only meaningful in Import mode -- same as
                    // GranularParamPage::Capture's own knob handling.
                    if(sample_synth_capture_source_ == -2)
                    {
                        int total = sample_synth_import_file_count_;
                        if(total > 0)
                        {
                            int idx = (int)(Clampf(k1, 0.f, 1.f) * total);
                            if(idx >= total)
                                idx = total - 1;
                            if(idx != sample_synth_import_cursor_)
                                sample_synth_capture_status_[0] = '\0';
                            sample_synth_import_cursor_ = idx;
                        }
                    }
                    break;
                }
                case SampleSynthParamPage::Trim:
                {
                    bool changed = false;
                    if(KnobPickUp(k1, k1_pickup_raw_[ci], k1_pickup_engaged_[ci]))
                    {
                        sample_synth_trim_start01_ = k1;
                        changed                     = true;
                    }
                    if(KnobPickUp(k2, k2_pickup_raw_[ci], k2_pickup_engaged_[ci]))
                    {
                        sample_synth_trim_end01_ = k2;
                        changed                   = true;
                    }
                    if(changed)
                        ApplySampleSynthTrim();
                    break;
                }
                case SampleSynthParamPage::Tune:
                    if(KnobPickUp(k1, k1_pickup_raw_[ci], k1_pickup_engaged_[ci]))
                    {
                        // Same +-24 semitone range/quantization as
                        // GranularEngine::SetGrainTuneSemitones01() --
                        // center (k1=0.5) is exactly 0 semitones, i.e.
                        // "play back exactly as captured," matching that
                        // page's own feel. root_note moves the OPPOSITE
                        // direction from semitones (read_inc =
                        // 2^((note-root)/12), so raising pitch for a
                        // fixed played note means LOWERING root) -- see
                        // SyncPickupTargets()'s own KnobContext::
                        // SampleSynthTune case for the matching inverse.
                        float v01     = Clampf(k1, 0.f, 1.f);
                        int   semis   = (int)(v01 * 48.f + 0.5f) - 24; // -24..+24
                        int   note    = 60 - semis;
                        sample_synth_tune_root_note_ = (uint8_t)Clampf((float)note, 0.f, 127.f);
                        ApplySampleSynthTrim();
                    }
                    if(KnobPickUp(k2, k2_pickup_raw_[ci], k2_pickup_engaged_[ci]))
                    {
                        // Tune for whatever's loaded via Import/Preset/
                        // User specifically -- see sample_synth_imp_tune_
                        // semis_'s own comment. Same +-24 range/
                        // quantization as K1 above, applied through
                        // SetSlotPitchOffsetSemis() (same mechanism
                        // Setup's own Tune knob already uses -- see that
                        // setter's own comment for why it works cleanly
                        // on chromatic content too).
                        float v01   = Clampf(k2, 0.f, 1.f);
                        int   semis = (int)(v01 * 48.f + 0.5f) - 24; // -24..+24
                        sample_synth_imp_tune_semis_ = (int8_t)semis;
                        if(sample_synth_voice_source_ == SampleSynthVoiceSource::Imported)
                            for(int si = 0; si < SampleSynth::kMaxSampleSlots; si++)
                                if(sample_synth_->SlotHasSample(si))
                                    sample_synth_->SetSlotPitchOffsetSemis(si, (int8_t)semis);
                    }
                    break;
                case SampleSynthParamPage::Import: break; // no continuous knobs
                case SampleSynthParamPage::Preset:
                {
                    // Direct "browse a list" idiom, no pickup -- same
                    // shape as Dexed/Grains' own Import file lists.
                    // At the bank level (sample_synth_sound_category_
                    // < 0) this same knob browses the bank list instead
                    // -- see sample_synth_sound_category_'s own comment.
                    if(sample_synth_sound_category_ < 0)
                    {
                        int n = sample_synth_bank_count_;
                        if(n > 0)
                        {
                            int idx = (int)(Clampf(k1, 0.f, 1.f) * n);
                            if(idx >= n)
                                idx = n - 1;
                            sample_synth_sound_category_cursor_ = idx;
                        }
                        break;
                    }
                    // Inside a folder K2 scrolls the sounds and K1 is left
                    // alone, so it still points at this folder on Back.
                    int total = sample_synth_sound_count_;
                    if(total > 0)
                    {
                        int idx = (int)(Clampf(k2, 0.f, 1.f) * total);
                        if(idx >= total)
                            idx = total - 1;
                        if(idx != sample_synth_kit_cursor_)
                            sample_synth_kit_status_[0] = '\0';
                        sample_synth_kit_cursor_ = idx;
                    }
                    break;
                }
                case SampleSynthParamPage::User:
                {
                    // Same "browse a list" idiom as Preset above, no
                    // category level -- whichever face's own User-
                    // preset list, see SampleSynthVoiceUserPreset/
                    // SampleSynthDrumUserPreset's own comments.
                    bool is_voice = sample_synth_mode_ == SampleSynthMode::Voice;
                    int& cursor = is_voice ? sample_synth_voice_user_cursor_
                                             : sample_synth_drum_user_cursor_;
                    int  count  = is_voice ? sample_synth_voice_user_count_
                                             : sample_synth_drum_user_count_;
                    char* status = is_voice ? sample_synth_voice_user_status_
                                              : sample_synth_drum_user_status_;
                    if(save_load_mode_ == SaveLoadMode::ChoosingSave)
                    {
                        // K1 picks Overwrite / Save New, same as every other
                        // engine's save choice (Overwrite needs a loaded slot).
                        int loaded = is_voice ? sample_synth_voice_user_loaded_slot_
                                              : sample_synth_drum_user_loaded_slot_;
                        save_as_new_ = loaded <= 0 ? true : k1 >= 0.5f;
                    }
                    if(count > 0 && save_load_mode_ == SaveLoadMode::BrowsingLoad)
                    {
                        int idx = (int)(Clampf(k2, 0.f, 1.f) * count);
                        if(idx >= count)
                            idx = count - 1;
                        if(idx != cursor)
                            status[0] = '\0';
                        cursor = idx;
                    }
                    break;
                }
                default: break;
            }
            break;
        }

        case Screen::DexedOperator:
        {
            if(!dexed_)
                break;
            int base = dexed_op_index_ * 21;
            switch(dexed_op_page_)
            {
                case DexedOpParamPage::Algo: break; // view-only here -- see its own comment
                case DexedOpParamPage::RatioLevel:
                    if(KnobPickUp(k1, k1_pickup_raw_[ci], k1_pickup_engaged_[ci]))
                    {
                        int coarse = (int)(Clampf(k1, 0.f, 1.f) * 32.f);
                        coarse     = coarse > 31 ? 31 : coarse;
                        if((uint8_t)coarse != dexed_->GetPatchByte(base + 18))
                            dexed_->SetPatchByte(base + 18, (uint8_t)coarse);
                    }
                    if(KnobPickUp(k2, k2_pickup_raw_[ci], k2_pickup_engaged_[ci]))
                    {
                        int level = (int)(Clampf(k2, 0.f, 1.f) * 99.f + 0.5f);
                        level     = level > 99 ? 99 : level;
                        if((uint8_t)level != dexed_->GetPatchByte(base + 16))
                            dexed_->SetPatchByte(base + 16, (uint8_t)level);
                    }
                    break;
                case DexedOpParamPage::Detune:
                    if(KnobPickUp(k1, k1_pickup_raw_[ci], k1_pickup_engaged_[ci]))
                    {
                        int detune = (int)(Clampf(k1, 0.f, 1.f) * 14.f + 0.5f);
                        detune     = detune > 14 ? 14 : detune;
                        if((uint8_t)detune != dexed_->GetPatchByte(base + 20))
                            dexed_->SetPatchByte(base + 20, (uint8_t)detune);
                    }
                    break;
                case DexedOpParamPage::EgRate:
                {
                    int off1 = dexed_op_egrate_target_sr_ ? base + 2 : base + 0;
                    int off2 = dexed_op_egrate_target_sr_ ? base + 3 : base + 1;
                    if(KnobPickUp(k1, k1_pickup_raw_[ci], k1_pickup_engaged_[ci]))
                    {
                        int rate = (int)(Clampf(k1, 0.f, 1.f) * 99.f + 0.5f);
                        rate     = rate > 99 ? 99 : rate;
                        if((uint8_t)rate != dexed_->GetPatchByte(off1))
                            dexed_->SetPatchByte(off1, (uint8_t)rate);
                    }
                    if(KnobPickUp(k2, k2_pickup_raw_[ci], k2_pickup_engaged_[ci]))
                    {
                        int rate = (int)(Clampf(k2, 0.f, 1.f) * 99.f + 0.5f);
                        rate     = rate > 99 ? 99 : rate;
                        if((uint8_t)rate != dexed_->GetPatchByte(off2))
                            dexed_->SetPatchByte(off2, (uint8_t)rate);
                    }
                    break;
                }
                case DexedOpParamPage::EgLevel:
                {
                    int off1 = dexed_op_eglevel_target_sr_ ? base + 6 : base + 4;
                    int off2 = dexed_op_eglevel_target_sr_ ? base + 7 : base + 5;
                    if(KnobPickUp(k1, k1_pickup_raw_[ci], k1_pickup_engaged_[ci]))
                    {
                        int level = (int)(Clampf(k1, 0.f, 1.f) * 99.f + 0.5f);
                        level     = level > 99 ? 99 : level;
                        if((uint8_t)level != dexed_->GetPatchByte(off1))
                            dexed_->SetPatchByte(off1, (uint8_t)level);
                    }
                    if(KnobPickUp(k2, k2_pickup_raw_[ci], k2_pickup_engaged_[ci]))
                    {
                        int level = (int)(Clampf(k2, 0.f, 1.f) * 99.f + 0.5f);
                        level     = level > 99 ? 99 : level;
                        if((uint8_t)level != dexed_->GetPatchByte(off2))
                            dexed_->SetPatchByte(off2, (uint8_t)level);
                    }
                    break;
                }
                case DexedOpParamPage::AmpModSens:
                    if(KnobPickUp(k1, k1_pickup_raw_[ci], k1_pickup_engaged_[ci]))
                    {
                        // Only bits 0-1 of this byte are ever read
                        // (patch[off+14] & 3, see msfa's own
                        // ampmodsenstab lookup), so only those 4 values
                        // are worth quantizing to.
                        int sens = (int)(Clampf(k1, 0.f, 1.f) * 3.f + 0.5f);
                        sens     = sens > 3 ? 3 : sens;
                        if((uint8_t)sens != (dexed_->GetPatchByte(base + 14) & 3))
                            dexed_->SetPatchByte(base + 14, (uint8_t)sens);
                    }
                    break;
                case DexedOpParamPage::VelocitySens:
                    if(KnobPickUp(k1, k1_pickup_raw_[ci], k1_pickup_engaged_[ci]))
                    {
                        // Real DX7 key velocity sensitivity range, read
                        // directly (unmasked) by ScaleVelocity().
                        int sens = (int)(Clampf(k1, 0.f, 1.f) * 7.f + 0.5f);
                        sens     = sens > 7 ? 7 : sens;
                        if((uint8_t)sens != dexed_->GetPatchByte(base + 15))
                            dexed_->SetPatchByte(base + 15, (uint8_t)sens);
                    }
                    break;
                case DexedOpParamPage::KeybRateScale:
                    if(KnobPickUp(k1, k1_pickup_raw_[ci], k1_pickup_engaged_[ci]))
                    {
                        // Real DX7 keyboard rate scaling range, read
                        // directly (unmasked) by ScaleRate().
                        int rs = (int)(Clampf(k1, 0.f, 1.f) * 7.f + 0.5f);
                        rs     = rs > 7 ? 7 : rs;
                        if((uint8_t)rs != dexed_->GetPatchByte(base + 13))
                            dexed_->SetPatchByte(base + 13, (uint8_t)rs);
                    }
                    break;
                default: break;
            }
            break;
        }

        case Screen::Mixer:
        {
            int ch = mixer_position_;
            if(ch >= kNumMixerChannels) // Scope stop -- no continuous knobs
                break;
            if(ch == kNumMixerChannels - 1) // Master: Volume + Reverb Size, no toggle
            {
                if(KnobPickUp(k1, k1_pickup_raw_[ci], k1_pickup_engaged_[ci]))
                {
                    master_volume01_ = Clampf(k1, 0.f, 1.f);
                    master_volume_   = powf(master_volume01_, 2.5f) * 1.43f;
                    if(master_volume_ < 0.f)
                        master_volume_ = 0.f;
                }
                if(KnobPickUp(k2, k2_pickup_raw_[ci], k2_pickup_engaged_[ci]))
                    reverb_size01_ = Clampf(k2, 0.f, 1.f);
            }
            else if(!mixer_target_reverb_)
            {
                if(KnobPickUp(k1, k1_pickup_raw_[ci], k1_pickup_engaged_[ci]))
                    MixerSetVolume01(ch, k1);
                if(KnobPickUp(k2, k2_pickup_raw_[ci], k2_pickup_engaged_[ci]))
                    MixerSetPan01(ch, k2);
            }
            else
            {
                if(KnobPickUp(k1, k1_pickup_raw_[ci], k1_pickup_engaged_[ci]))
                    MixerSetSend01(ch, k1);
            }
            break;
        }
    }
}

void Ui::UpdateLeds()
{
    // True hardware PWM (led1_green_pwm_/led1_blue_pwm_/led2_red_pwm_,
    // set once by main.cpp's SetLedPwmChannels() -- see its own comment)
    // instead of libDaisy's software-PWM RgbLed/Led::Update() path this
    // function used to drive (fixed ~120Hz, Talbot-Plateau-limited at
    // low duty -- only ever safe at full brightness, which is why this
    // used to just call pod_->UpdateLeds() directly). Flat 40%/25% duty,
    // no gamma correction -- just two levels, on or off. Null-guarded
    // since these stay null for the brief window before main()'s setup
    // reaches SetLedPwmChannels().
    //
    // Scope deliberately narrowed to what these 3 specific sub-pixels can
    // show -- see led_pwm_tim3's own comment (main.cpp) for why only
    // these 3 of the Pod's 6 RGB sub-pixels got hardware PWM at all; the
    // other 3 (LED1-Red, LED2-Green, LED2-Blue) are simply never driven
    // again and stay off. This replaces both LED1's old per-cursor-layer
    // 7-state readout (Empty/ArmedCountIn/Recording/Overdubbing/
    // ArmedOverdubCountIn/Playing/Paused) and LED2's old Bypass-on/off
    // indicator -- a real loss of that information, not just a brightness
    // change, kept deliberately simple for this specific hardware-PWM A/B
    // rather than trying to cram the old 7-state meaning onto 2 colors.
    if(!led1_green_pwm_ || !led1_blue_pwm_ || !led2_red_pwm_)
        return;

    constexpr float kLedPwmLevel      = 0.4f;
    // Green reads a little brighter than blue at the same duty cycle
    // (real, reported) -- its own slightly lower level rather than
    // retuning the shared constant, since blue wasn't flagged.
    constexpr float kLedPwmLevelGreen = 0.32f;
    // Red reads brighter than green/blue at the same duty cycle (real,
    // reported: "a little too bright" at 0.4) -- its own lower level
    // rather than retuning the shared constant, since green/blue weren't
    // flagged.
    constexpr float kLedPwmLevelRed   = 0.25f;

    // Led1: looper transport, collapsed to the 3 states that matter at a
    // glance -- disabled (off), paused (blue), playing (green). Derived
    // from each layer's own real LayerState, not a separate bookkeeping
    // flag -- correctly shows "off" when every layer is still Empty
    // (nothing recorded yet) instead of claiming "playing."
    bool any_playing = false, any_paused = false;
    for(int i = 0; i < num_layers_; i++)
    {
        LayerState s = layers_[i].GetState();
        if(s == LayerState::Playing)
            any_playing = true;
        else if(s == LayerState::Paused)
            any_paused = true;
    }
    if(!looper_enabled_)
    {
        led1_green_pwm_->Set(0.f);
        led1_blue_pwm_->Set(0.f);
    }
    else if(any_playing)
    {
        led1_green_pwm_->Set(kLedPwmLevelGreen);
        led1_blue_pwm_->Set(0.f);
    }
    else if(any_paused)
    {
        led1_green_pwm_->Set(0.f);
        led1_blue_pwm_->Set(kLedPwmLevel);
    }
    else
    {
        // Nothing playing or paused -- every layer Empty, or mid-
        // Recording/count-in (Led2 covers the recording case).
        led1_green_pwm_->Set(0.f);
        led1_blue_pwm_->Set(0.f);
    }

    // Led2: one unified "something is actively recording" indicator --
    // any loop layer actually recording/overdubbing (not just armed/
    // counting in -- that's "about to," not "is"), or either Capture
    // pipeline (SampleSynth's or Grains') mid-capture.
    bool any_loop_recording = false;
    for(int i = 0; i < num_layers_; i++)
    {
        LayerState s = layers_[i].GetState();
        if(s == LayerState::Recording || s == LayerState::Overdubbing)
        {
            any_loop_recording = true;
            break;
        }
    }
    bool recording = any_loop_recording
                      || (granular_capturing_ && *granular_capturing_)
                      || (sample_synth_capturing_ && *sample_synth_capturing_);
    led2_red_pwm_->Set(recording ? kLedPwmLevelRed : 0.f);
}

// --- Drawing --------------------------------------------------------------

void Ui::Draw()
{
    disp_->Fill(false);
    switch(screen_)
    {
        case Screen::Home: DrawHome(); break;
        case Screen::Layer: DrawLayerScreen(); break;
        case Screen::Global: DrawGlobalScreen(); break;
        case Screen::Granular: DrawGranularScreen(); break;
        case Screen::Mixer: DrawMixerScreen(); break;
        case Screen::Dexed: DrawDexedScreen(); break;
        case Screen::Ensemble: DrawEnsembleScreen(); break;
        case Screen::DexedOperator: DrawDexedOperatorScreen(); break;
        case Screen::SampleSynth: DrawSampleSynthScreen(); break;
    }
    disp_->Update();
}

void Ui::WriteUpper(const char* text)
{
    char   buf[64];
    size_t n = strlen(text);
    if(n >= sizeof(buf))
        n = sizeof(buf) - 1;
    for(size_t i = 0; i < n; i++)
        buf[i] = (char)toupper((unsigned char)text[i]);
    buf[n] = '\0';
    disp_->WriteString(buf, Font_6x8, true);
}

void Ui::DrawControlRow(int         row_y,
                         bool       square_icon,
                         int        divider_y,
                         const char* left_label,
                         const char* left_value,
                         const char* right_value,
                         const char* right_label)
{
    // Geometry verified pixel-by-pixel against the real font metrics
    // (see the chat history this was designed in, or font_tomthumb.h):
    // baseline at the row's bottom pixel (NOT row_h-2 -- that crashes
    // tall glyphs into the divider above), icon vertically centered on
    // the rows text actually occupies (row_y..row_y+4, not the full
    // row_h-tall cell, which would sit the icon low against the margin).
    constexpr int kRowH      = kTomThumbRowHeight; // 6
    constexpr int kIconR     = 2;
    constexpr int kClearance = 2;
    constexpr int kMargin    = 1;

    if(divider_y >= 0)
        disp_->DrawLine(0, divider_y, disp_->Width() - 1, divider_y, true);

    const int baseline = row_y + kRowH - 1;
    const int icon_cx  = disp_->Width() / 2;
    const int icon_cy  = row_y + 2;

    TomThumbDrawText(disp_, kMargin, baseline, left_label, true);
    if(right_label[0] != '\0')
    {
        int rw = TomThumbAdvanceWidth(right_label);
        TomThumbDrawText(disp_, disp_->Width() - kMargin - rw, baseline, right_label, true);
    }

    if(left_value[0] != '\0')
    {
        int left_ink_w         = TomThumbInkWidth(left_value);
        int left_exclusive_end = icon_cx - kIconR - kClearance;
        TomThumbDrawText(disp_, left_exclusive_end - left_ink_w, baseline, left_value, true);
    }
    if(right_value[0] != '\0')
    {
        int right_start = icon_cx + kIconR + kClearance + 1;
        TomThumbDrawText(disp_, right_start, baseline, right_value, true);
    }

    if(square_icon)
        disp_->DrawRect(icon_cx - kIconR, icon_cy - kIconR, icon_cx + kIconR,
                         icon_cy + kIconR, true, false);
    else
        disp_->DrawCircle(icon_cx, icon_cy, kIconR, true);
}

void Ui::DrawBeatIndicator(int x, int y, int dot_size)
{
    // Reserve fixed space for the "B<n>" label -- up to "B16" (3 chars)
    // -- whether or not it's actually drawn this call, so the dots
    // never shift position as the bar number's digit count changes
    // (e.g. "B9" -> "B10") mid-performance.
    if(tempo_->GetBars() > 1)
    {
        char label[6];
        snprintf(label, sizeof(label), "B%d", tempo_->GetBarInLoop() + 1);
        disp_->SetCursor(x, y);
        WriteUpper(label);
    }
    int dots_x = x + 3 * 6 + 3; // 3 reserved chars @ Font_6x8 + a small gap

    const int gap = dot_size >= 5 ? 3 : 2;
    int       beat = tempo_->GetBeatInBar();
    for(int i = 0; i < TempoClock::kBeatsPerBar; i++)
    {
        bool filled = (i == beat);
        disp_->DrawRect(
            dots_x, y, dots_x + dot_size - 1, y + dot_size - 1, true, filled);
        dots_x += dot_size + gap;
    }
}

void Ui::DrawHome()
{
    // Beat position, prominent (dot_size=6) -- this is the primary
    // "where am I" readout. BPM/Bars/Metro share its row in Tom Thumb
    // (verified to fit: the beat indicator -- 3-char "B<n>" reserve plus
    // 4 dots -- occupies x2-55, leaving ~70px, and "BPM:240 BARS:16 [M]"
    // (worst case: max BPM, max Bars) is 67px) instead of getting its own
    // row below.
    DrawBeatIndicator(2, 0, 6);

    char line[24];
    snprintf(line, sizeof(line), "BPM:%d BARS:%d %s",
              (int)(tempo_->GetBpm() + 0.5f), tempo_->GetBars(),
              tempo_->IsMetronomeEnabled() ? "[M]" : "[ ]");
    // Right-aligned, 1px from the edge -- same margin convention as the
    // footer rows. Worst case (max BPM/Bars) is 67px, so this still
    // clears the beat indicator's right edge (x55) with room to spare.
    int line_w = TomThumbAdvanceWidth(line);
    TomThumbDrawText(disp_, disp_->Width() - 1 - line_w, kTomThumbRowHeight - 1, line, true);

    // Layer boxes, sized to fill the full display width evenly no
    // matter how many layers there are -- kNumLayers has changed once
    // already (5 -> 4) and might again, so this shouldn't hard-code a
    // box width tuned for one specific count.
    const int box_gap = 4;
    const int box_w = (disp_->Width() - (num_layers_ - 1) * box_gap) / num_layers_;
    // Vertically centered in the space between the top row's content
    // (beat dots + BPM/Bars/Metro text, rows 0-5) and the footer divider
    // (row kFooterDividerY=46) -- that's 40 rows (6..45) for a 24px box,
    // so top=14 splits the remaining 16px into an 8px gap on each side.
    const int box_h = 24, top = 14;
    for(int i = 0; i < num_layers_; i++)
    {
        int x0 = i * (box_w + box_gap);
        int x1 = x0 + box_w - 1;
        int y1 = top + box_h - 1;
        bool selected = (i == cursor_layer_);
        disp_->DrawRect(x0, top, x1, y1, true, false);
        if(selected)
            disp_->DrawRect(x0 + 1, top + 1, x1 - 1, y1 - 1, true, false);

        char label[8];
        snprintf(label, sizeof(label), "%d%s", i + 1, StateGlyph(layers_[i].GetState()));
        disp_->SetCursor(x0 + 4, top + 8);
        WriteUpper(label);
    }

    // Master Volume/Metronome Volume shown live from their actual
    // values, NOT k1_pickup_raw_/k2_pickup_raw_ -- those track the raw
    // knob position for the pickup/catch mechanism (see KnobPickUp()'s
    // comment in ui.h) and only update once a knob is actually caught,
    // so on first landing here after a layer switch they'd still be
    // sitting at their stale default instead of the real value.
    char vol_val[8], metro_val[8];
    snprintf(vol_val, sizeof(vol_val), "%d%%", (int)(master_volume01_ * 100.f + 0.5f));
    snprintf(metro_val, sizeof(metro_val), "%d%%",
              (int)(tempo_->GetMetronomeVolume01() * 100.f + 0.5f));
    DrawControlRow(kFooterRow1Y, false, kFooterDividerY, "Vol", vol_val, metro_val, "Metro");

    // Button1's label reflects what a tap/hold will actually do right now
    // (see LooperLayer::OnRecordButtonPressed/LongPress) rather than a
    // static "Rec/Play" covering three different behaviours -- once
    // there's content, a tap toggles Pause/Play and a hold starts an
    // overdub, both unchanged; this only changes what the label says.
    const char* rec_label = "Rec";
    switch(layers_[cursor_layer_].GetState())
    {
        case LayerState::Empty: rec_label = "Rec"; break;
        case LayerState::Playing:
        case LayerState::Paused: rec_label = "Pause/Overdub"; break;
        case LayerState::ArmedCountIn: rec_label = "Count-in"; break;
        case LayerState::Recording: rec_label = "Stop"; break;
        // A tap now ends overdub early (same as Recording's own "Stop"
        // above), now that overdub runs a full pass automatically
        // instead of lasting exactly as long as the button stays held.
        case LayerState::Overdubbing: rec_label = "Stop"; break;
        case LayerState::ArmedOverdubCountIn: rec_label = "Count-in"; break;
    }
    char byp_label[12];
    snprintf(byp_label, sizeof(byp_label), "Bypass:%s", bypass_ ? "On" : "Off");
    DrawControlRow(kFooterRow2Y, true, kFooterInterRowDividerY, rec_label, "", "", byp_label);
}

// Downsampled peak-bar waveform + optional moving playhead tick, shared
// by Layer:Status (one layer's own peaks/position) and Global:Speed (a
// composite max across all 4 layers -- see DrawSpeedScreen()). Centered
// in the full gap between the title divider (y9) and the footer divider
// (kFooterDividerY), 1px bar + 1px gap. Auto-scaled to the loudest of the
// 63 cached buckets, so quiet input still uses the full height instead of
// reading as "barely there" -- cheap to do here (63 floats, ~30Hz redraw)
// versus the buffer itself (up to 1.6M raw samples), which is exactly why
// the cache exists in the first place.
void Ui::DrawWaveform(const float* peaks, bool draw_playhead, float playhead_pos01)
{
    float max_peak = 0.f;
    for(int col = 0; col < LooperLayer::kWaveformCols; col++)
        if(peaks[col] > max_peak)
            max_peak = peaks[col];
    float scale = max_peak > 0.001f ? 1.f / max_peak : 0.f;

    const int kBandTop    = 14;
    const int kBandBottom = 44;
    const int center_y    = (kBandTop + kBandBottom) / 2;
    const int half_h      = (kBandBottom - kBandTop) / 2;
    for(int col = 0; col < LooperLayer::kWaveformCols; col++)
    {
        int x = 1 + col * 2;
        int h = (int)(Clampf(peaks[col] * scale, 0.f, 1.f) * half_h + 0.5f);
        if(h <= 0)
            disp_->DrawPixel(x, center_y, true);
        else
            disp_->DrawLine(x, center_y - h, x, center_y + h, true);
    }
    if(draw_playhead)
    {
        int col = (int)(playhead_pos01 * LooperLayer::kWaveformCols);
        if(col >= LooperLayer::kWaveformCols)
            col = LooperLayer::kWaveformCols - 1;
        int px = 1 + col * 2;
        disp_->DrawLine(px, 10, px, 11, true);
    }
}

// Same downsampled peak-bar technique as DrawWaveform() above, but with
// TWO live markers (the Grain layer's fixed anchor, the Scan layer's
// sweeping one) and its own vertical geometry -- this page also shows a
// live Size/Fill/Gap/Scan text readout above the waveform, which
// DrawWaveform()'s other callers don't need, so the band starts lower
// (kBandTop=24 vs 14) to leave room for it.
void Ui::DrawGranularWaveform(const float* peaks, float grain_anchor01, float scan_anchor01,
                                bool has_source, float grain_size01, float range_start01,
                                float range_end01)
{
    float max_peak = 0.f;
    for(int col = 0; col < GranularEngine::kWaveformCols; col++)
        if(peaks[col] > max_peak)
            max_peak = peaks[col];
    float scale = max_peak > 0.001f ? 1.f / max_peak : 0.f;

    // kBandTop pushed down to 27 (vs DrawWaveform()'s 14) -- this page's
    // header is now two Tom Thumb rows (label + centered value) instead
    // of one, ending around y21, so this needs the extra clearance.
    const int kBandTop    = 27;
    const int kBandBottom = 44;
    const int center_y    = (kBandTop + kBandBottom) / 2;
    const int half_h      = (kBandBottom - kBandTop) / 2;
    for(int col = 0; col < GranularEngine::kWaveformCols; col++)
    {
        int x = 1 + col * 2;
        int h = (int)(Clampf(peaks[col] * scale, 0.f, 1.f) * half_h + 0.5f);
        if(h <= 0)
            disp_->DrawPixel(x, center_y, true);
        else
            disp_->DrawLine(x, center_y - h, x, center_y + h, true);
    }
    if(has_source)
    {
        // Scan's marker sits one row above Grain's -- kept visually
        // distinct even when both anchors land on the same column
        // (e.g. right after NoteOn, before Scan has swept away).
        int col_s = (int)(Clampf(scan_anchor01, 0.f, 1.f) * GranularEngine::kWaveformCols);
        if(col_s >= GranularEngine::kWaveformCols)
            col_s = GranularEngine::kWaveformCols - 1;
        int px_s = 1 + col_s * 2;
        disp_->DrawLine(px_s, kBandTop - 4, px_s, kBandTop - 3, true);

        int col_g = (int)(Clampf(grain_anchor01, 0.f, 1.f) * GranularEngine::kWaveformCols);
        if(col_g >= GranularEngine::kWaveformCols)
            col_g = GranularEngine::kWaveformCols - 1;
        int px_g = 1 + col_g * 2;
        if(grain_size01 > 0.f)
        {
            // Span the marker to actually show Size, centered on the
            // anchor -- a horizontal bar this wide plus a 1px center
            // tick, instead of the plain point marker below (which
            // Trim's own reuse of this function still gets, via
            // grain_size01's default of 0 -- start/end points there
            // don't have a "size" to show).
            int half_span_px = (int)(grain_size01 * (float)GranularEngine::kWaveformCols + 0.5f);
            int left_px       = px_g - half_span_px;
            int right_px      = px_g + half_span_px;
            int max_x = 1 + (GranularEngine::kWaveformCols - 1) * 2;
            if(left_px < 1)
                left_px = 1;
            if(right_px > max_x)
                right_px = max_x;
            disp_->DrawLine(left_px, kBandTop - 2, right_px, kBandTop - 2, true);
            disp_->DrawPixel(px_g, kBandTop - 1, true);
        }
        else
        {
            disp_->DrawLine(px_g, kBandTop - 2, px_g, kBandTop - 1, true);
        }
    }
    if(range_start01 >= 0.f && range_end01 >= 0.f)
    {
        // Taller than the waveform band itself -- runs from just below
        // the header divider (y9) down through the anchor-marker zone
        // and the whole waveform, so these read as real boundary markers
        // rather than blending into the waveform's own bars.
        const int kRangeTop = 11;
        int col_a = (int)(Clampf(range_start01, 0.f, 1.f) * GranularEngine::kWaveformCols);
        if(col_a >= GranularEngine::kWaveformCols)
            col_a = GranularEngine::kWaveformCols - 1;
        int col_b = (int)(Clampf(range_end01, 0.f, 1.f) * GranularEngine::kWaveformCols);
        if(col_b >= GranularEngine::kWaveformCols)
            col_b = GranularEngine::kWaveformCols - 1;
        disp_->DrawLine(1 + col_a * 2, kRangeTop, 1 + col_a * 2, kBandBottom, true);
        disp_->DrawLine(1 + col_b * 2, kRangeTop, 1 + col_b * 2, kBandBottom, true);
    }
}

void Ui::DrawLayerScreen()
{
    char title[32];
    const char* page_name = "Status";
    switch(layer_page_)
    {
        case LayerPage::Status: page_name = "Status"; break;
        case LayerPage::Speed: page_name = "Speed"; break;
        case LayerPage::Filter: page_name = "Filter"; break;
        case LayerPage::Effect: page_name = "Effect"; break;
        case LayerPage::Reverb: page_name = "LYR-FX"; break;
        case LayerPage::Gain: page_name = "Gain"; break;
        default: break;
    }
    // Compact "N:Page" (not "Layer N - Page") specifically to leave room
    // for the beat indicator on the same row -- see DrawBeatIndicator().
    snprintf(title, sizeof(title), "%d:%s", cursor_layer_ + 1, page_name);
    disp_->SetCursor(0, 0);
    WriteUpper(title);
    DrawBeatIndicator(disp_->Width() - 41, 0, 3);
    disp_->DrawLine(0, 9, disp_->Width() - 1, 9, true);

    char line1[32];
    switch(layer_page_)
    {
        case LayerPage::Status:
        {
            LayerState st = Cur().GetState();

            // Long-press-to-clear takes over this whole body area instead
            // of sharing cramped space with the waveform -- it's a
            // destructive action, so it gets the user's full attention
            // while they're deciding whether to keep holding.
            if(pod_->button2.Pressed())
            {
                float held = pod_->button2.TimeHeldMs();
                int   w    = (int)(Clampf(held / 800.f, 0.f, 1.f) * (disp_->Width() - 2));
                const char* msg   = "Hold to clear...";
                int         msg_w = (int)strlen(msg) * 6; // Font_6x8
                disp_->SetCursor((disp_->Width() - msg_w) / 2, 16);
                WriteUpper(msg);
                disp_->DrawRect(0, 28, disp_->Width() - 1, 42, true, false);
                if(w > 0)
                    disp_->DrawRect(1, 29, w, 41, true, true);
            }
            else if(undo_status_[0] != '\0'
                    && System::GetNow() - undo_status_set_ms_ < kUndoStatusTimeoutMs)
            {
                // Same "takes over the body" treatment as "Hold to
                // clear..." above -- brief, rare (only shown when Undo
                // was tapped with nothing pending). Unlike this file's other
                // transient status_ messages, this one can't just wait for
                // some unrelated action to overwrite it -- it squats on the
                // waveform, the one thing an idle user is actually looking
                // at on this page -- so it times out on its own instead.
                int w = (int)strlen(undo_status_) * 6; // Font_6x8
                disp_->SetCursor((disp_->Width() - w) / 2, 24);
                WriteUpper(undo_status_);
            }
            else
            {
                // Downsampled waveform with a moving playhead tick above
                // it (see LooperLayer::GetWaveformPeaks()/GetPlayPos01()),
                // replacing the old meter bar -- and the state heading
                // that used to live here is gone too, since the button
                // row's label below is state-dependent and says it
                // instead. See DrawWaveform() (shared with Global:Speed's
                // composite view) for the actual drawing/auto-scaling.
                bool draw_ph = (st == LayerState::Playing || st == LayerState::Paused
                                || st == LayerState::Overdubbing
                                || st == LayerState::ArmedOverdubCountIn);
                DrawWaveform(Cur().GetWaveformPeaks(), draw_ph, Cur().GetPlayPos01());
            }

            // Actual values, not the pickup-tracking array -- see the
            // comment on DrawHome()'s equivalent Vol readout.
            char vol_val[8], pan_val[8];
            snprintf(vol_val, sizeof(vol_val), "%d%%", (int)(Cur().GetVolume01() * 100.f + 0.5f));
            snprintf(pan_val, sizeof(pan_val), "%d%%", (int)(Cur().GetPan01() * 100.f + 0.5f));
            DrawControlRow(kFooterRow1Y, false, kFooterDividerY, "Vol", vol_val, pan_val, "Pan");

            // Label reflects what tap/hold actually do right now (see
            // LooperLayer::OnRecordButtonPressed/LongPress) -- split
            // Playing/Paused apart (unlike Home's combined label) so it
            // says which of Pause/Play a tap will do, not just that one
            // of them will happen.
            const char* rec_label = "Rec";
            switch(st)
            {
                case LayerState::Empty: rec_label = "Rec"; break;
                case LayerState::ArmedCountIn: rec_label = "Cancel"; break;
                case LayerState::Recording: rec_label = "Stop"; break;
                // "ShortPressFn/LongPressFn" order, consistently -- see
                // the Button2 label right below for the matching
                // "Undo/Clear" convention on this same row.
                case LayerState::Playing: rec_label = "Pause/Overdub"; break;
                case LayerState::Paused: rec_label = "Play/Overdub"; break;
                // "Stop", not "Overdubbing" -- a tap now ends it early
                // (same as Recording's own "Stop" above), now that
                // overdub runs a full pass automatically instead of
                // lasting exactly as long as the button stays held.
                case LayerState::Overdubbing: rec_label = "Stop"; break;
                case LayerState::ArmedOverdubCountIn: rec_label = "Cancel"; break;
            }
            // "Undo/Clear" -- short tap Undoes the last overdub (see
            // TriggerUndoOverdub()), 800ms hold Clears the whole layer.
            DrawControlRow(kFooterRow2Y, true, kFooterInterRowDividerY, rec_label, "", "", "Undo/Clear");
            break;
        }
        case LayerPage::Speed:
        {
            // Live readout instead of a static instruction -- also fixes
            // this line running past the display's ~21-char width at
            // 6px/char on a 128px screen (same class of bug as the
            // Reverb page's "(shared)" getting clipped).
            // %f is used nowhere in this file on purpose: this build
            // links --specs=nano.specs, whose snprintf doesn't support
            // float conversions without extra flash we'd rather not
            // spend on it, so it silently prints nothing for %f. Integer
            // whole/hundredths math instead, same trick used for BPM etc.
            int speed_x100 = (int)(Cur().GetSpeed() * 100.f + 0.5f);
            snprintf(line1, sizeof(line1), "Speed: %d.%02dx", speed_x100 / 100,
                      speed_x100 % 100);
            disp_->SetCursor(0, 20);
            WriteUpper(line1);

            char speed_val[10];
            snprintf(speed_val, sizeof(speed_val), "%d.%02dx", speed_x100 / 100,
                      speed_x100 % 100);
            // Knob2 does nothing on this page -- "" draws nothing rather
            // than a fake value (see DrawControlRow()'s comment).
            DrawControlRow(kFooterRow1Y, false, kFooterDividerY, "Speed", speed_val, "", "");
            DrawControlRow(kFooterRow2Y, true, kFooterInterRowDividerY, "Reset", "", "", "");
            break;
        }
        case LayerPage::Filter:
        {
            snprintf(line1, sizeof(line1), "Mode: %s", FilterModeName(Cur().GetFilterMode()));
            disp_->SetCursor(0, 20);
            WriteUpper(line1);

            // Actual values, not the pickup-tracking array -- see the
            // comment on DrawHome()'s equivalent Vol readout.
            char cutoff_val[8], res_val[8];
            snprintf(cutoff_val, sizeof(cutoff_val), "%d%%",
                      (int)(Cur().GetFilterCutoff01() * 100.f + 0.5f));
            snprintf(res_val, sizeof(res_val), "%d%%",
                      (int)(Cur().GetFilterResonance01() * 100.f + 0.5f));
            DrawControlRow(kFooterRow1Y, false, kFooterDividerY, "Cutoff", cutoff_val, res_val, "Res");
            DrawControlRow(kFooterRow2Y, true, kFooterInterRowDividerY, "Cycle mode", "", "", "");
            break;
        }
        case LayerPage::Effect:
        {
            snprintf(line1, sizeof(line1), "FX: %s", EffectName(Cur().GetEffect()));
            disp_->SetCursor(0, 20);
            WriteUpper(line1);

            // EffectParamA/BLabel() return "-" for a knob this effect
            // doesn't use (e.g. Drive ignores ParamB) -- treat that as
            // "does nothing" too, same as Speed/Gain's idle knob2.
            const char* a_label = EffectParamALabel(Cur().GetEffect());
            const char* b_label = EffectParamBLabel(Cur().GetEffect());
            // Actual values, not the pickup-tracking array -- see the
            // comment on DrawHome()'s equivalent Vol readout.
            char a_val[8] = "", b_val[8] = "";
            if(strcmp(a_label, "-") != 0)
                snprintf(a_val, sizeof(a_val), "%d%%",
                          (int)(Cur().GetEffectParamA01() * 100.f + 0.5f));
            if(strcmp(b_label, "-") != 0)
                snprintf(b_val, sizeof(b_val), "%d%%",
                          (int)(Cur().GetEffectParamB01() * 100.f + 0.5f));
            DrawControlRow(kFooterRow1Y, false, kFooterDividerY,
                             strcmp(a_label, "-") == 0 ? "" : a_label, a_val,
                             b_val, strcmp(b_label, "-") == 0 ? "" : b_label);
            DrawControlRow(kFooterRow2Y, true, kFooterInterRowDividerY, "Cycle FX", "", "", "");
            break;
        }
        case LayerPage::Reverb:
        {
            // Send only for both effects -- Size/Time/Feedback are shared
            // Global:FX settings now (every layer sends into the same
            // ONE reverb bus and ONE delay bus, see the shared-bus
            // comment on LooperLayer::SetReverbSend01()/SetDelaySend01()).
            char rev_val[8], delay_val[8];
            snprintf(rev_val, sizeof(rev_val), "%d%%",
                      (int)(Cur().GetReverbSend01() * 100.f + 0.5f));
            snprintf(delay_val, sizeof(delay_val), "%d%%",
                      (int)(Cur().GetDelaySend01() * 100.f + 0.5f));
            DrawControlRow(kFooterRow1Y, false, kFooterDividerY, "Reverb", rev_val, delay_val,
                             "Delay");
            DrawControlRow(kFooterRow2Y, true, kFooterInterRowDividerY, "", "", "", "");
            break;
        }
        case LayerPage::Gain:
        {
            // See the Speed page's comment above on why not %f.
            int gain_x10 = (int)(Cur().GetInputGain() * 10.f + 0.5f);
            snprintf(line1, sizeof(line1), "Gain: %d.%dx", gain_x10 / 10, gain_x10 % 10);
            disp_->SetCursor(0, 20);
            WriteUpper(line1);

            char gain_val[8];
            snprintf(gain_val, sizeof(gain_val), "%d.%dx", gain_x10 / 10, gain_x10 % 10);
            DrawControlRow(kFooterRow1Y, false, kFooterDividerY, "Gain", gain_val, "", "");
            DrawControlRow(kFooterRow2Y, true, kFooterInterRowDividerY, "", "", "", "");
            break;
        }
        default: break;
    }
}

void Ui::DrawGlobalScreen()
{
    if(global_page_ == GlobalPage::File)
    {
        DrawFileScreen();
        return;
    }
    if(global_page_ == GlobalPage::SdMgmt)
    {
        DrawSdMgmtScreen();
        return;
    }
    if(global_page_ == GlobalPage::Export)
    {
        DrawExportScreen();
        return;
    }
    if(global_page_ == GlobalPage::Speed)
    {
        DrawSpeedScreen();
        return;
    }
    if(global_page_ == GlobalPage::Granular)
    {
        // Entry point into Screen::Granular, plus the on/off toggle --
        // same treatment as Global:Pad above.
        disp_->SetCursor(0, 0);
        WriteUpper("Global:Grains");
        DrawBeatIndicator(disp_->Width() - 41, 0, 3);
        disp_->DrawLine(0, 9, disp_->Width() - 1, 9, true);
        disp_->SetCursor(0, 20);
        WriteUpper(granular_enabled_ ? "Enabled" : "Disabled");
        disp_->SetCursor(0, 30);
        WriteUpper("Click to open Grains");
        DrawControlRow(kFooterRow1Y, false, kFooterDividerY, "", "", "", "");
        DrawControlRow(kFooterRow2Y, true, kFooterInterRowDividerY, "Toggle On/Off", "", "", "");
        return;
    }
    if(global_page_ == GlobalPage::Looper)
    {
        // Toggle only, no screen to drill into -- see IsLooperEnabled()'s
        // own comment for the pause-vs-full-stop distinction.
        disp_->SetCursor(0, 0);
        WriteUpper("Global:Looper");
        DrawBeatIndicator(disp_->Width() - 41, 0, 3);
        disp_->DrawLine(0, 9, disp_->Width() - 1, 9, true);
        disp_->SetCursor(0, 20);
        WriteUpper(looper_enabled_ ? "Enabled" : "Disabled");
        DrawControlRow(kFooterRow1Y, false, kFooterDividerY, "", "", "", "");
        DrawControlRow(kFooterRow2Y, true, kFooterInterRowDividerY, "Toggle On/Off", "", "", "");
        return;
    }
    if(global_page_ == GlobalPage::Mixer)
    {
        // Entry point only, same treatment as Global:Pad/Granular -- an
        // at-a-glance summary (same 8-channel grid used elsewhere, see
        // DrawMixerOverviewGrid()) plus a hint that clicking drops into
        // the real editing surface, drawn as real content in the body
        // (not disguised as a Button1/Button2 footer label -- this is
        // about the ENCODER, and DrawControlRow's row grammar specifically
        // means "this label describes the button next to it"). No knobs
        // of its own any more.
        disp_->SetCursor(0, 0);
        WriteUpper("Global:Mixer");
        DrawBeatIndicator(disp_->Width() - 41, 0, 3);
        disp_->DrawLine(0, 9, disp_->Width() - 1, 9, true);

        DrawMixerOverviewGrid(15);
        {
            const char* hint = "PUSH ENC TO ENTER";
            int         hw   = TomThumbAdvanceWidth(hint);
            TomThumbDrawText(disp_, (disp_->Width() - hw) / 2, 44, hint, true);
        }

        // Live CPU load -- both footer rows were otherwise empty on this
        // page (no knobs/buttons of their own), so this costs no extra
        // screen space. Avg is a continuously-smoothed reading (see
        // daisy::CpuLoadMeter's own 1-pole filter); Peak rolls over
        // every ~10s (see main.cpp's kCpuPeakResetSeconds) so it reads
        // fresh again shortly after you change something, rather than
        // staying stuck on the worst moment from several minutes ago.
        {
            char avg_val[8], peak_val[8];
            snprintf(avg_val, sizeof(avg_val), "%d%%", (int)(g_cpu_avg_load * 100.f + 0.5f));
            snprintf(peak_val, sizeof(peak_val), "%d%%", (int)(g_cpu_peak_load * 100.f + 0.5f));
            DrawControlRow(kFooterRow1Y, false, kFooterDividerY, "CPU", avg_val, peak_val,
                             "Peak");
        }
        DrawControlRow(kFooterRow2Y, true, kFooterInterRowDividerY, "", "", "", "");
        return;
    }
    if(global_page_ == GlobalPage::Dexed)
    {
        // Entry point into Screen::Dexed, plus the on/off toggle -- same
        // treatment as Global:Granular above.
        disp_->SetCursor(0, 0);
        WriteUpper("Global:Dexed");
        DrawBeatIndicator(disp_->Width() - 41, 0, 3);
        disp_->DrawLine(0, 9, disp_->Width() - 1, 9, true);
        disp_->SetCursor(0, 20);
        WriteUpper(dexed_enabled_ ? "Enabled" : "Disabled");
        disp_->SetCursor(0, 30);
        WriteUpper("Click to open Dexed");
        DrawControlRow(kFooterRow1Y, false, kFooterDividerY, "", "", "", "");
        DrawControlRow(kFooterRow2Y, true, kFooterInterRowDividerY, "Toggle On/Off", "", "", "");
        return;
    }
    if(global_page_ == GlobalPage::Ensemble)
    {
        // Entry point into Screen::Ensemble, plus the on/off toggle --
        // same treatment as Global:Granular/Global:Dexed above.
        disp_->SetCursor(0, 0);
        // "Ensmbl" not "Ensemble" -- same budget as Global:Grains'
        // own shortened label (title row reserves the right 41px for
        // the beat indicator, ~14 chars max in Font_6x8 before it
        // overlaps -- "Global:Ensemble" didn't fit, confirmed on real
        // hardware).
        WriteUpper("Global:Ensmbl");
        DrawBeatIndicator(disp_->Width() - 41, 0, 3);
        disp_->DrawLine(0, 9, disp_->Width() - 1, 9, true);
        disp_->SetCursor(0, 20);
        WriteUpper(ensemble_enabled_ ? "Enabled" : "Disabled");
        disp_->SetCursor(0, 30);
        WriteUpper("Click to open Ensmbl");
        DrawControlRow(kFooterRow1Y, false, kFooterDividerY, "", "", "", "");
        DrawControlRow(kFooterRow2Y, true, kFooterInterRowDividerY, "Toggle On/Off", "", "", "");
        return;
    }
    if(global_page_ == GlobalPage::SampleSynthVoice)
    {
        // Entry point into Screen::SampleSynth (Voice face), plus the
        // on/off toggle -- same treatment as Global:Granular/Dexed/
        // Ensemble above, but mutually exclusive with SMPDR below (see
        // sample_synth_voice_enabled_'s own comment).
        disp_->SetCursor(0, 0);
        WriteUpper("Global:SMPVO");
        DrawBeatIndicator(disp_->Width() - 41, 0, 3);
        disp_->DrawLine(0, 9, disp_->Width() - 1, 9, true);
        disp_->SetCursor(0, 20);
        WriteUpper(sample_synth_voice_enabled_ ? "Enabled" : "Disabled");
        disp_->SetCursor(0, 30);
        WriteUpper("Click to open SMPVO");
        DrawControlRow(kFooterRow1Y, false, kFooterDividerY, "", "", "", "");
        DrawControlRow(kFooterRow2Y, true, kFooterInterRowDividerY, "Toggle On/Off", "", "", "");
        return;
    }
    if(global_page_ == GlobalPage::SampleSynthDrum)
    {
        disp_->SetCursor(0, 0);
        WriteUpper("Global:SMPDR");
        DrawBeatIndicator(disp_->Width() - 41, 0, 3);
        disp_->DrawLine(0, 9, disp_->Width() - 1, 9, true);
        disp_->SetCursor(0, 20);
        WriteUpper(sample_synth_drum_enabled_ ? "Enabled" : "Disabled");
        disp_->SetCursor(0, 30);
        WriteUpper("Click to open SMPDR");
        DrawControlRow(kFooterRow1Y, false, kFooterDividerY, "", "", "", "");
        DrawControlRow(kFooterRow2Y, true, kFooterInterRowDividerY, "Toggle On/Off", "", "", "");
        return;
    }

    // Compact form (not "Global Settings") for the same reason as the
    // Layer screen's title -- leaves room for the beat indicator on the
    // same row.
    const char* title = "Global:Tempo";
    if(global_page_ == GlobalPage::Filter)
        title = "Global:Filter";
    else if(global_page_ == GlobalPage::Reverb)
        title = "Global:GLB-FX";
    else if(global_page_ == GlobalPage::ReverbTone)
        title = "Global:RevTone";
    else if(global_page_ == GlobalPage::ReverbDiffusion)
        title = "Global:RevDiff";
    disp_->SetCursor(0, 0);
    WriteUpper(title);
    DrawBeatIndicator(disp_->Width() - 41, 0, 3);
    disp_->DrawLine(0, 9, disp_->Width() - 1, 9, true);

    char line1[32], line2[32];
    if(global_page_ == GlobalPage::Tempo)
    {
        // Held Button2 takes over this space with a hold-to-confirm
        // progress bar, same pattern as Global:File's Hold=New/Load --
        // see TriggerSaveDefaults()'s comment for why this is a hold,
        // not a tap.
        if(pod_->button2.Pressed())
        {
            float held = pod_->button2.TimeHeldMs();
            int   w    = (int)(Clampf(held / 800.f, 0.f, 1.f) * (disp_->Width() - 2));
            disp_->SetCursor(0, 20);
            WriteUpper("Hold: Default...");
            disp_->DrawRect(0, 30, disp_->Width() - 1, 34, true, false);
            if(w > 0)
                disp_->DrawRect(1, 31, w, 33, true, true);
            DrawControlRow(kFooterRow1Y, false, kFooterDividerY, "", "", "", "");
            DrawControlRow(kFooterRow2Y, true, kFooterInterRowDividerY, "", "", "", "");
            return;
        }

        // Bars right-aligned on the same row as BPM -- it was previously
        // the only thing on this page not shown in the "big" Font_6x8
        // text, only as a small footer readout. "(locked)" dropped from
        // here since the "Clear layers first" line below already says
        // the same thing when it's relevant, and dropping it keeps this
        // row's left side free of a variable-length suffix that would
        // otherwise sometimes collide with the right-aligned Bars text.
        snprintf(line1, sizeof(line1), "BPM: %d", (int)(tempo_->GetBpm() + 0.5f));
        char bars_line[16];
        // Shown as "current/max", not just the number -- Bars' own
        // ceiling now depends on BPM (see TempoClock::MaxBarsForBpm()),
        // so without this the knob can visibly stop climbing with no
        // explanation of why.
        snprintf(bars_line, sizeof(bars_line), "Bars: %d/%d", tempo_->GetBars(),
                  TempoClock::MaxBarsForBpm(tempo_->GetBpm()));
        snprintf(line2, sizeof(line2), "Metro: %s",
                  tempo_->IsMetronomeEnabled() ? "On" : "Off");
        disp_->SetCursor(0, 12);
        WriteUpper(line1);
        // Font_6x8 is fixed-width (6px/char), so right-aligning is a
        // plain strlen() * 6 -- same trick DrawExportScreen() uses.
        disp_->SetCursor(disp_->Width() - 6 * (int)strlen(bars_line), 12);
        WriteUpper(bars_line);
        disp_->SetCursor(0, 24);
        WriteUpper(line2);
        // Same row either way -- the "save as default" result (if any)
        // is a direct response to something the user just did, so it
        // takes priority over the locked hint on the rare chance both
        // are true at once.
        if(tempo_status_[0] != '\0')
        {
            disp_->SetCursor(0, 36);
            WriteUpper(tempo_status_);
        }
        else if(tempo_->IsLocked())
        {
            disp_->SetCursor(0, 36);
            WriteUpper("Clear layers first");
        }

        char bpm_val[8], bars_val[8];
        snprintf(bpm_val, sizeof(bpm_val), "%d", (int)(tempo_->GetBpm() + 0.5f));
        snprintf(bars_val, sizeof(bars_val), "%d", tempo_->GetBars());
        DrawControlRow(kFooterRow1Y, false, kFooterDividerY, "BPM", bpm_val, bars_val, "Bars");
        DrawControlRow(kFooterRow2Y, true, kFooterInterRowDividerY, "Metro on/off", "", "",
                         "Hold=Default");
    }
    else if(global_page_ == GlobalPage::Filter) // master-bus filter, applied
         // once to the full mix in main.cpp, not per-layer like the Layer
         // Filter page.
    {
        snprintf(line1, sizeof(line1), "Mode: %s", FilterModeName(master_filter_mode_));
        disp_->SetCursor(0, 16);
        WriteUpper(line1);

        // Actual values, not the pickup-tracking array -- see the
        // comment on DrawHome()'s equivalent Vol readout.
        char cutoff_val[8], res_val[8];
        snprintf(cutoff_val, sizeof(cutoff_val), "%d%%",
                  (int)(master_filter_cutoff01_ * 100.f + 0.5f));
        snprintf(res_val, sizeof(res_val), "%d%%", (int)(master_filter_res01_ * 100.f + 0.5f));
        DrawControlRow(kFooterRow1Y, false, kFooterDividerY, "Cutoff", cutoff_val, res_val, "Res");
        DrawControlRow(kFooterRow2Y, true, kFooterInterRowDividerY, "Cycle mode", "", "", "");
    }
    else if(global_page_ == GlobalPage::Reverb && !global_fx_target_delay_)
    // Reverb pair -- Size/decay for the ONE reverb bus every layer's
    // Send feeds (see LooperLayer::SetReverbSend01()'s comment for why
    // this isn't per-layer any more), plus Bypass's own independent
    // Send into that same bus.
    {
        char size_val[8], byp_send_val[8];
        snprintf(size_val, sizeof(size_val), "%d%%", (int)(reverb_size01_ * 100.f + 0.5f));
        snprintf(byp_send_val, sizeof(byp_send_val), "%d%%",
                  (int)(bypass_reverb_send01_ * 100.f + 0.5f));
        DrawControlRow(kFooterRow1Y, false, kFooterDividerY, "Size", size_val, byp_send_val,
                         "Byp Send");
        DrawControlRow(kFooterRow2Y, true, kFooterInterRowDividerY,
                         "Reverb*", "", "", "Delay");
    }
    else if(global_page_ == GlobalPage::Reverb) // global_fx_target_delay_ -- Delay
         // pair, shared Time/Feedback for the ONE cross-feedback delay bus
         // both Dexed and Grains send into (see main.cpp's fx_delay_l/r).
    {
        char time_val[8], fb_val[8];
        snprintf(time_val, sizeof(time_val), "%d%%", (int)(delay_time01_ * 100.f + 0.5f));
        snprintf(fb_val, sizeof(fb_val), "%d%%", (int)(delay_feedback01_ * 100.f + 0.5f));
        DrawControlRow(kFooterRow1Y, false, kFooterDividerY, "Time", time_val, fb_val,
                         "Feedback");
        DrawControlRow(kFooterRow2Y, true, kFooterInterRowDividerY,
                         "Reverb", "", "", "Delay*");
    }
    else if(global_page_ == GlobalPage::ReverbTone && !global_reverb_tone_target_b_)
    // PreDelay + PreFilter pair -- gap before the tail starts, and input
    // tone/brightness going into the tank (see DattorroReverb).
    {
        char predelay_val[8], prefilter_val[8];
        snprintf(predelay_val, sizeof(predelay_val), "%d%%",
                  (int)(reverb_predelay01_ * 100.f + 0.5f));
        snprintf(prefilter_val, sizeof(prefilter_val), "%d%%",
                  (int)(reverb_prefilter01_ * 100.f + 0.5f));
        DrawControlRow(kFooterRow1Y, false, kFooterDividerY, "PreDelay", predelay_val,
                         prefilter_val, "PreFilt");
        DrawControlRow(kFooterRow2Y, true, kFooterInterRowDividerY,
                         "PreDelay*", "", "", "Decay/Damp");
    }
    else if(global_page_ == GlobalPage::ReverbTone) // global_reverb_tone_target_b_ --
         // DecayDiffusion + Damping pair -- tail density/smoothness and
         // brightness-over-time.
    {
        char decdiff_val[8], damping_val[8];
        snprintf(decdiff_val, sizeof(decdiff_val), "%d%%",
                  (int)(reverb_decay_diffusion01_ * 100.f + 0.5f));
        snprintf(damping_val, sizeof(damping_val), "%d%%",
                  (int)(reverb_damping01_ * 100.f + 0.5f));
        DrawControlRow(kFooterRow1Y, false, kFooterDividerY, "DecDiff", decdiff_val,
                         damping_val, "Damping");
        DrawControlRow(kFooterRow2Y, true, kFooterInterRowDividerY,
                         "PreDelay", "", "", "Decay/Damp*");
    }
    else // GlobalPage::ReverbDiffusion -- Input Diffusion 1+2, no toggle
         // needed (only 2 knobs left). Shapes how smeared/soft the attack
         // transient is going into the reverb.
    {
        char diff1_val[8], diff2_val[8];
        snprintf(diff1_val, sizeof(diff1_val), "%d%%",
                  (int)(reverb_input_diffusion1_01_ * 100.f + 0.5f));
        snprintf(diff2_val, sizeof(diff2_val), "%d%%",
                  (int)(reverb_input_diffusion2_01_ * 100.f + 0.5f));
        DrawControlRow(kFooterRow1Y, false, kFooterDividerY, "Diff 1", diff1_val, diff2_val,
                         "Diff 2");
        DrawControlRow(kFooterRow2Y, true, kFooterInterRowDividerY, "", "", "", "");
    }
}

void Ui::DrawFileScreen()
{
    disp_->SetCursor(0, 0);
    WriteUpper("Global:File");
    DrawBeatIndicator(disp_->Width() - 41, 0, 3);
    disp_->DrawLine(0, 9, disp_->Width() - 1, 9, true);

    if(!PerformanceStore::IsCardPresent())
    {
        // Centered the same way Global:Export's own "No SD card" is --
        // see its own comment for the exact math.
        const char* msg = "No SD card";
        int         x   = (disp_->Width() - 6 * (int)strlen(msg)) / 2;
        disp_->SetCursor(x < 0 ? 0 : x, 24);
        WriteUpper(msg);
        DrawControlRow(kFooterRow1Y, false, kFooterDividerY, "", "", "", "");
        DrawControlRow(kFooterRow2Y, true, kFooterInterRowDividerY, "Retry", "", "", "");
        return;
    }

    if(file_slots_dirty_)
        RefreshFileSlots();

    // Whichever button is actually held wins the space (hold-to-confirm
    // progress, same idea as the Status page's Clear); otherwise this
    // area shows whichever of Idle/ChoosingSave/BrowsingLoad is active
    // (see SaveLoadMode's own comment).
    bool choosing_save = save_load_mode_ == SaveLoadMode::ChoosingSave;
    bool browsing_load = save_load_mode_ == SaveLoadMode::BrowsingLoad;
    // Load's own top-level chooser (Files/New, mirrors ChoosingSave's own
    // Overwrite/Save New) -- see load_new_selected_/load_browsing_files_'s
    // own comment in ui.h.
    bool load_chooser  = browsing_load && !load_browsing_files_;
    bool can_hold_load = browsing_load && (load_browsing_files_ || load_new_selected_);
    // Button1 has no hold action any more -- its tap means "Save" (Idle),
    // "drill into the file list" (Load chooser with Files highlighted),
    // or "Back" (everywhere else revealed, see OnButton1Short()). Button2
    // confirms Save and Load (see HandleButton2()) once there's something
    // specific to confirm.
    if(pod_->button2.Pressed() && (choosing_save || can_hold_load))
    {
        float       held     = pod_->button2.TimeHeldMs();
        int         w        = (int)(Clampf(held / 800.f, 0.f, 1.f) * (disp_->Width() - 2));
        const char* hold_msg = choosing_save ? "Hold: Save..." : "Hold: Load...";
        disp_->SetCursor(0, 20);
        WriteUpper(hold_msg);
        disp_->DrawRect(0, 30, disp_->Width() - 1, 34, true, false);
        if(w > 0)
            disp_->DrawRect(1, 31, w, 33, true, true);
    }
    else if(choosing_save)
    {
        // Overwrite only shown as a real option once something's loaded
        // -- Knob1's own ApplyKnobs() handling already forces Save New
        // when it isn't, this just reflects that on screen too.
        bool can_overwrite = loaded_slot_ >= 0;
        char line1[24], line2[16];
        snprintf(line1, sizeof(line1), "%c Overwrite%s", !save_as_new_ ? '>' : ' ',
                  can_overwrite ? "" : " (n/a)");
        snprintf(line2, sizeof(line2), "%c Save New", save_as_new_ ? '>' : ' ');
        disp_->SetCursor(0, 16);
        WriteUpper(line1);
        disp_->SetCursor(0, 28);
        WriteUpper(line2);
    }
    else if(load_chooser)
    {
        // Same shape as ChoosingSave's own Overwrite/Save New -- Files
        // only a real option once something's actually saved, same
        // "can't select what doesn't exist" reasoning as Overwrite's own.
        char line1[24], line2[16];
        snprintf(line1, sizeof(line1), "%c Files%s", !load_new_selected_ ? '>' : ' ',
                  file_slot_count_ > 0 ? "" : " (n/a)");
        snprintf(line2, sizeof(line2), "%c Load New", load_new_selected_ ? '>' : ' ');
        disp_->SetCursor(0, 16);
        WriteUpper(line1);
        disp_->SetCursor(0, 28);
        WriteUpper(line2);
    }
    else if(browsing_load)
    {
        // Drilled into the numbered list (load_browsing_files_ == true).
        // "PERF" matches the exact short code Global:SdMgmt's own grid
        // box uses (SdMgmtFolderShortLabel()) -- one consistent
        // vocabulary for "which engine is this slot from" everywhere
        // a bare number is shown, not a second one invented here.
        char line2[32];
        snprintf(line2, sizeof(line2), "Load: PERF%d", file_slots_[file_cursor_]);
        disp_->SetCursor(0, 20);
        WriteUpper(line2);
    }
    else
    {
        char line1[32];
        if(loaded_slot_ >= 0)
            snprintf(line1, sizeof(line1), "Now: PERF%d", loaded_slot_);
        else
            snprintf(line1, sizeof(line1), "Now: (unsaved)");
        disp_->SetCursor(0, 20);
        WriteUpper(line1);

        if(file_status_[0] != '\0')
        {
            disp_->SetCursor(0, 32);
            WriteUpper(file_status_);
        }
    }

    const char* b1_label;
    if(save_load_mode_ == SaveLoadMode::Idle)
        b1_label = "Save";
    else if(load_chooser && !load_new_selected_)
        b1_label = "Select"; // drills into the numbered list
    else
        b1_label = "Back";
    const char* b2_label;
    if(save_load_mode_ == SaveLoadMode::Idle)
        b2_label = "Load";
    else if(choosing_save)
        b2_label = "Hold=Save";
    else if(can_hold_load)
        b2_label = "Hold=Load";
    else
        b2_label = ""; // Load chooser with Files highlighted -- nothing to confirm yet

    DrawControlRow(kFooterRow1Y, false, kFooterDividerY,
                     (choosing_save || browsing_load) ? "Scroll" : "", "", "", "");
    DrawControlRow(kFooterRow2Y, true, kFooterInterRowDividerY, b1_label, "", "", b2_label);
}

const char* Ui::SdMgmtFolderName(SdMgmtFolder f)
{
    switch(f)
    {
        case SdMgmtFolder::Performances: return "Performances";
        case SdMgmtFolder::GranularPresets: return "Grains Presets";
        case SdMgmtFolder::DexedPresets: return "Dexed Presets";
        case SdMgmtFolder::EnsemblePresets: return "Ensemble Presets";
        case SdMgmtFolder::SampleSynthCaptures: return "Sampler Captures";
        case SdMgmtFolder::SampleSynthKits: return "Sampler Kits";
        case SdMgmtFolder::SampleSynthVoiceUserPresets: return "SMPVO User";
        case SdMgmtFolder::SampleSynthDrumUserPresets: return "SMPDR User";
        default: return "?";
    }
}

const char* Ui::SdMgmtFolderShortLabel(SdMgmtFolder f)
{
    switch(f)
    {
        case SdMgmtFolder::Performances: return "PERF";
        case SdMgmtFolder::GranularPresets: return "GRAINS";
        case SdMgmtFolder::DexedPresets: return "DEXED";
        case SdMgmtFolder::EnsemblePresets: return "ENSM";
        case SdMgmtFolder::SampleSynthCaptures: return "CAPT";
        case SdMgmtFolder::SampleSynthKits: return "KITS";
        case SdMgmtFolder::SampleSynthVoiceUserPresets: return "SMPVO";
        case SdMgmtFolder::SampleSynthDrumUserPresets: return "SMPDR";
        default: return "?";
    }
}

void Ui::DrawSdMgmtScreen()
{
    disp_->SetCursor(0, 0);
    WriteUpper("Global:SD Mgmt");
    DrawBeatIndicator(disp_->Width() - 41, 0, 3);
    disp_->DrawLine(0, 9, disp_->Width() - 1, 9, true);

    if(!PerformanceStore::IsCardPresent())
    {
        // Centered the same way Global:File/Export's own "No SD card" is.
        const char* msg = "No SD card";
        int         x   = (disp_->Width() - 6 * (int)strlen(msg)) / 2;
        disp_->SetCursor(x < 0 ? 0 : x, 24);
        WriteUpper(msg);
        DrawControlRow(kFooterRow1Y, false, kFooterDividerY, "", "", "", "");
        DrawControlRow(kFooterRow2Y, true, kFooterInterRowDividerY, "Retry", "", "", "");
        return;
    }

    // Whichever button is actually held wins the space, same hold-to-
    // confirm-progress idiom as every other destructive/blocking action.
    if(pod_->button1.Pressed() && sd_mgmt_in_folder_
       && !(SdMgmtIsBankFolder() && sd_mgmt_bank_ < 0))
    {
        float held = pod_->button1.TimeHeldMs();
        int   w    = (int)(Clampf(held / 400.f, 0.f, 1.f) * (disp_->Width() - 2));
        disp_->SetCursor(0, 20);
        WriteUpper("Hold: Duplicate...");
        disp_->DrawRect(0, 30, disp_->Width() - 1, 34, true, false);
        if(w > 0)
            disp_->DrawRect(1, 31, w, 33, true, true);
        DrawControlRow(kFooterRow1Y, false, kFooterDividerY, "", "", "", "");
        DrawControlRow(kFooterRow2Y, true, kFooterInterRowDividerY, "", "", "", "");
        return;
    }
    if(pod_->button2.Pressed() && sd_mgmt_in_folder_
       && !(SdMgmtIsBankFolder() && sd_mgmt_bank_ < 0))
    {
        float held = pod_->button2.TimeHeldMs();
        int   w    = (int)(Clampf(held / kSdMgmtDeleteHoldMs, 0.f, 1.f) * (disp_->Width() - 2));
        disp_->SetCursor(0, 20);
        WriteUpper("Hold: Delete...");
        disp_->DrawRect(0, 30, disp_->Width() - 1, 34, true, false);
        if(w > 0)
            disp_->DrawRect(1, 31, w, 33, true, true);
        DrawControlRow(kFooterRow1Y, false, kFooterDividerY, "", "", "", "");
        DrawControlRow(kFooterRow2Y, true, kFooterInterRowDividerY, "", "", "", "");
        return;
    }

    if(!sd_mgmt_in_folder_)
    {
        // 4x2 grid, one box per SdMgmtFolder entry (row-major, see that
        // enum's own doc comment) -- replaced the original vertical
        // ">"-highlighted list once there were 8 real categories to
        // show instead of 4; a flat list that long would have needed
        // either a scrolling window or illegibly cramped rows, where a
        // grid fits all of them on screen at once. Flush edge-to-edge
        // (no gap between cells) -- at 32x18px per box there's no
        // pixel to spare, and adjoining borders simply double up into
        // one shared line, same look as a plain spreadsheet grid.
        constexpr int kCols   = 4, kRows = 2;
        constexpr int kBodyTop = 10; // just below the header divider at y=9
        constexpr int kCellW  = 128 / kCols; // 32 -- exact, disp_->Width() is always 128 here
        constexpr int kCellH  = (kFooterDividerY - kBodyTop) / kRows; // 18
        int n = (int)SdMgmtFolder::kCount;
        for(int i = 0; i < n; i++)
        {
            int col = i % kCols;
            int row = i / kCols;
            int x0  = col * kCellW;
            int y0  = kBodyTop + row * kCellH;
            int x1  = x0 + kCellW - 1;
            int y1  = y0 + kCellH - 1;
            bool selected = i == (int)sd_mgmt_folder_;
            // Selected box: filled, label drawn in the "off" color so it
            // reads as dark-on-light -- same DrawRect(..., true, true)
            // then off-color-text idiom as every other inverted-
            // selection highlight would need on a 1-bit display; every
            // other box just gets an outline.
            disp_->DrawRect(x0, y0, x1, y1, true, selected);
            const char* label = SdMgmtFolderShortLabel((SdMgmtFolder)i);
            int         lw    = TomThumbAdvanceWidth(label);
            int         lx    = x0 + (kCellW - lw) / 2;
            int         baseline = y0 + kCellH / 2 + 2; // visually centers a 5px-tall glyph
            TomThumbDrawText(disp_, lx < x0 ? x0 : lx, baseline, label, !selected);
        }
    }
    else
    {
        char line1[24];
        bool bank_box = SdMgmtIsBankFolder();
        int  bank_level_count = bank_box ? SdMgmtBankLevelCount() : 0;
        if(bank_box && sd_mgmt_cursor_ >= bank_level_count)
            sd_mgmt_cursor_ = bank_level_count > 0 ? bank_level_count - 1 : 0;
        if(bank_box && sd_mgmt_bank_ >= 0)
        {
            char label[16];
            SampleBankEntryLabel(sd_mgmt_banks_[sd_mgmt_bank_], label, sizeof(label));
            snprintf(line1, sizeof(line1), "%s:", label);
        }
        else
            snprintf(line1, sizeof(line1), "%s:", SdMgmtFolderName(sd_mgmt_folder_));
        disp_->SetCursor(0, 14);
        WriteUpper(line1);

        if(sd_mgmt_status_[0] != '\0')
        {
            disp_->SetCursor(0, 26);
            WriteUpper(sd_mgmt_status_);
        }
        else if(bank_box)
        {
            char line2[24];
            if(bank_level_count == 0)
                snprintf(line2, sizeof(line2), "(empty)");
            else if(sd_mgmt_bank_ < 0)
                SampleBankEntryLabel(sd_mgmt_banks_[sd_mgmt_cursor_], line2, sizeof(line2));
            else
                snprintf(line2, sizeof(line2), "%s", sd_mgmt_item_names_[sd_mgmt_cursor_]);
            disp_->SetCursor(0, 26);
            WriteUpper(line2);
        }
        else
        {
            int        count = 0;
            const int* slots = SdMgmtSlots(&count);
            char       line2[16];
            if(slots && count > 0)
                snprintf(line2, sizeof(line2), "%d", slots[sd_mgmt_cursor_]);
            else
                snprintf(line2, sizeof(line2), "(empty)");
            disp_->SetCursor(0, 26);
            WriteUpper(line2);
        }
    }

    DrawControlRow(kFooterRow1Y, false, kFooterDividerY, "Scroll", "", "", "");
    // "Back/Duplicate" -- ShortPressFn/LongPressFn, same convention as
    // Layer:Status's "Pause/Overdub"/"Undo/Clear". Delete stays
    // "Hold=Delete" (Button2 has no short-tap meaning in-folder here).
    bool at_bank_list = sd_mgmt_in_folder_ && SdMgmtIsBankFolder() && sd_mgmt_bank_ < 0;
    DrawControlRow(kFooterRow2Y, true, kFooterInterRowDividerY,
                     sd_mgmt_in_folder_ ? (at_bank_list ? "Back" : "Back/Duplicate") : "Select",
                     "", "",
                     sd_mgmt_in_folder_ ? (at_bank_list ? "Open" : "Hold=Delete") : "");
}

void Ui::DrawExportScreen()
{
    disp_->SetCursor(0, 0);
    WriteUpper("Global:Export");
    DrawBeatIndicator(disp_->Width() - 41, 0, 3);
    disp_->DrawLine(0, 9, disp_->Width() - 1, 9, true);

    // One line, centered in the space between the title divider (y9) and
    // the footer divider (kFooterDividerY=46) -- 36 rows (10..45) for an
    // 8px-tall Font_6x8 line, so y=24 splits the remaining 28px evenly.
    // Font_6x8 is fixed-width (6px/char), so horizontal centering is a
    // plain strlen() * 6.
    const char* msg = PerformanceStore::IsCardPresent()
                           ? (export_status_[0] != '\0' ? export_status_ : "Export WAV File")
                           : "No SD card";
    int x = (disp_->Width() - 6 * (int)strlen(msg)) / 2;
    disp_->SetCursor(x < 0 ? 0 : x, 24);
    WriteUpper(msg);

    if(!PerformanceStore::IsCardPresent())
    {
        DrawControlRow(kFooterRow1Y, false, kFooterDividerY, "", "", "", "");
        DrawControlRow(kFooterRow2Y, true, kFooterInterRowDividerY, "Retry", "", "", "");
        return;
    }

    // No hold-to-confirm here (unlike File's New/Load) -- Export always
    // creates a new numbered file and never overwrites/destroys anything,
    // so a plain tap is safe.
    DrawControlRow(kFooterRow1Y, false, kFooterDividerY, "", "", "", "");
    DrawControlRow(kFooterRow2Y, true, kFooterInterRowDividerY, "STUDIO 48K", "", "", "CD 44.1K");
}

void Ui::DrawSpeedScreen()
{
    disp_->SetCursor(0, 0);
    WriteUpper("Global:Speed");
    DrawBeatIndicator(disp_->Width() - 41, 0, 3);
    disp_->DrawLine(0, 9, disp_->Width() - 1, 9, true);

    // Composite waveform (max across all 4 layers' own caches, not just
    // the cursor layer) -- matches vari-speed/scrub's whole premise that
    // everything on the shared loop moves together, one tape. Playhead
    // comes from the lowest-indexed non-empty layer's own position --
    // there's no separate tracked "shared position" (each layer's
    // play_pos_ is independent state, kept roughly in sync only by
    // sharing record_len_ and starting at 0), and scrub nudges already
    // write directly into that same layer's play_pos_ (see ScrubBy()), so
    // this reflects live scrub movement automatically with no special
    // casing here.
    float composite_peaks[LooperLayer::kWaveformCols];
    for(int col = 0; col < LooperLayer::kWaveformCols; col++)
    {
        float m = 0.f;
        for(int L = 0; L < num_layers_; L++)
        {
            const float* p = layers_[L].GetWaveformPeaks();
            if(p[col] > m)
                m = p[col];
        }
        composite_peaks[col] = m;
    }
    int rep = -1;
    for(int L = 0; L < num_layers_; L++)
    {
        if(layers_[L].HasContent())
        {
            rep = L;
            break;
        }
    }
    DrawWaveform(composite_peaks, rep >= 0, rep >= 0 ? layers_[rep].GetPlayPos01() : 0.f);

    char speed_val[10];
    int  speed_x100 = (int)(project_speed_ * 100.f + 0.5f);
    snprintf(speed_val, sizeof(speed_val), "%d.%02dx", speed_x100 / 100, speed_x100 % 100);
    DrawControlRow(kFooterRow1Y, false, kFooterDividerY, "Speed", speed_val, "", "");
    const char* transport_label = speed_transport_mode_ == SpeedTransportMode::Scrub
                                       ? "Mode:Scrub"
                                       : speed_transport_mode_ == SpeedTransportMode::Freeze
                                             ? "Mode:Freeze"
                                             : "Mode:Off";
    DrawControlRow(kFooterRow2Y, true, kFooterInterRowDividerY,
                     transport_label, "", "", "Reset");
}

void Ui::DrawGranularScreen()
{
    if(!granular_)
        return;

    const char* page_name = "Grain";
    switch(granular_param_page_)
    {
        case GranularParamPage::Grain: page_name = "Grain"; break;
        case GranularParamPage::Position: page_name = "POS+RHY"; break;
        case GranularParamPage::ScanRange: page_name = "ScanRng"; break;
        case GranularParamPage::Scan: page_name = "Scan"; break;
        case GranularParamPage::Tune: page_name = "Tune"; break;
        case GranularParamPage::ADSR: page_name = "ADSR"; break;
        case GranularParamPage::Filter: page_name = "Filter"; break;
        case GranularParamPage::Mix: page_name = "Mix"; break;
        case GranularParamPage::FX: page_name = "GR-FX"; break;
        case GranularParamPage::MIDI: page_name = "MIDI"; break;
        case GranularParamPage::Capture: page_name = "Capture"; break;
        case GranularParamPage::Trim: page_name = "Trim"; break;
        case GranularParamPage::Preset: page_name = "Preset"; break;
        default: break;
    }
    // Shared content band for the simpler pages below (ADSR's graph,
    // Filter's oscilloscope) -- same values as Pad's own kBandTop/
    // kBandBottom. Grain's own page uses a deeper band (see
    // DrawGranularWaveform()) since it also has the two-row Size/Fill/
    // Gap/Scan header above it that these pages don't.
    const int kBandTop = 22, kBandBottom = 44;
    char title[24];
    snprintf(title, sizeof(title), "Grains:%s", page_name);
    disp_->SetCursor(0, 0);
    WriteUpper(title);
    DrawBeatIndicator(disp_->Width() - 41, 0, 3);
    disp_->DrawLine(0, 9, disp_->Width() - 1, 9, true);

    switch(granular_param_page_)
    {
        case GranularParamPage::Grain:
        {
            // Live values for all four params, always shown regardless of
            // which pair the knobs currently target -- same "readout
            // shows everything, buttons only change what the knobs
            // reach" idiom as Pad's own merged ADSR page. Tom Thumb (not
            // the big Font_6x8) so all four fit as a proper label-over-
            // value layout instead of one cramped abbreviated line; four
            // equal-width columns so they land evenly across the display,
            // and each value is centered on its own label's center (via
            // TomThumbAdvanceWidth()), not just left-aligned under it, so
            // differently-wide values (e.g. "3" vs "100%") still read as
            // belonging to the word above them.
            const char* grain_labels[4] = {"SIZE", "FILL", "GAP", "JITR"};
            char        grain_values[4][8];
            snprintf(grain_values[0], sizeof(grain_values[0]), "%d%%",
                      (int)(granular_->GetSize01() * 100.f + 0.5f));
            snprintf(grain_values[1], sizeof(grain_values[1]), "%d", granular_->GetFill());
            snprintf(grain_values[2], sizeof(grain_values[2]), "%d%%",
                      (int)(granular_->GetGap01() * 100.f + 0.5f));
            snprintf(grain_values[3], sizeof(grain_values[3]), "%d%%",
                      (int)(granular_->GetJitter01() * 100.f + 0.5f));
            const int kGrainLabelBaseline = 15;
            const int kGrainValueBaseline = 21;
            const int kGrainColWidth      = disp_->Width() / 4;
            for(int i = 0; i < 4; i++)
            {
                int center_x = kGrainColWidth * i + kGrainColWidth / 2;
                int lw       = TomThumbAdvanceWidth(grain_labels[i]);
                TomThumbDrawText(disp_, center_x - lw / 2, kGrainLabelBaseline, grain_labels[i],
                                  true);
                int vw = TomThumbAdvanceWidth(grain_values[i]);
                TomThumbDrawText(disp_, center_x - vw / 2, kGrainValueBaseline, grain_values[i],
                                  true);
            }

            DrawGranularWaveform(granular_->GetWaveformPeaks(), granular_->GetGrainAnchor01(),
                                  granular_->GetScanAnchor01(), granular_->HasSource(),
                                  granular_->GetGrainSizeFraction01());

            if(!granular_grain_target_gap_jitter_)
            {
                char sz_val[8], fl_val[8];
                snprintf(sz_val, sizeof(sz_val), "%d%%",
                          (int)(granular_->GetSize01() * 100.f + 0.5f));
                snprintf(fl_val, sizeof(fl_val), "%d", granular_->GetFill());
                DrawControlRow(kFooterRow1Y, false, kFooterDividerY, "Size", sz_val, fl_val,
                                 "Fill");
            }
            else
            {
                char gp_val[8], jt_val[8];
                snprintf(gp_val, sizeof(gp_val), "%d%%",
                          (int)(granular_->GetGap01() * 100.f + 0.5f));
                snprintf(jt_val, sizeof(jt_val), "%d%%",
                          (int)(granular_->GetJitter01() * 100.f + 0.5f));
                DrawControlRow(kFooterRow1Y, false, kFooterDividerY, "Gap", gp_val, jt_val,
                                 "Jitter");
            }
            // Marks which pair Button1/Button2 currently map the knobs
            // to, same "*" convention as Pad's own ADSR page.
            DrawControlRow(kFooterRow2Y, true, kFooterInterRowDividerY,
                             granular_grain_target_gap_jitter_ ? "SzFl" : "SzFl*", "", "",
                             granular_grain_target_gap_jitter_ ? "GpJt*" : "GpJt");
            break;
        }
        case GranularParamPage::Position:
        {
            // Same full-sample waveform + live anchor markers as the
            // Grain page (just DrawGranularWaveform() again -- it already
            // draws both the Grain layer's anchor and the Scan layer's),
            // so turning either knob visibly moves its own marker across
            // the sample instead of only being a number.
            //
            // Rhythm (Button1) and Speed (Button2) both share this page
            // as button-cycled modes -- same "top text shows the
            // button-controlled mode" treatment as Filter's "Mode: X"
            // and Capture's "Source: X", just two of them since there
            // are two buttons doing mode-cycling duty here now.
            char line1[24];
            snprintf(line1, sizeof(line1), "Rhy:%s  Spd:%s", granular_->GetRhythmName(),
                      granular_->GetGrainSpeedName());
            TomThumbDrawText(disp_, 0, 15, line1, true);

            DrawGranularWaveform(granular_->GetWaveformPeaks(), granular_->GetGrainAnchor01(),
                                  granular_->GetScanAnchor01(), granular_->HasSource(),
                                  granular_->GetGrainSizeFraction01());

            char pos_val[8];
            snprintf(pos_val, sizeof(pos_val), "%d%%",
                      (int)(granular_->GetPosition01() * 100.f + 0.5f));
            DrawControlRow(kFooterRow1Y, false, kFooterDividerY, "Position", pos_val,
                             GranularDirectionName(granular_->GetDirection()), "Dir");
            // Button hints, same "left label = Button1, right label =
            // Button2" idiom as DexedOperator's own "Op"/"AD/SR" row.
            DrawControlRow(kFooterRow2Y, true, kFooterInterRowDividerY, "Rhythm", "", "",
                             "Speed");
            break;
        }
        case GranularParamPage::ScanRange:
        {
            // Same waveform + anchor markers as every other Grains page,
            // plus full-height Start/End boundary lines so the sweep's
            // actual range is visible directly on the display, not just
            // as two percentages.
            DrawGranularWaveform(granular_->GetWaveformPeaks(), granular_->GetGrainAnchor01(),
                                  granular_->GetScanAnchor01(), granular_->HasSource(),
                                  granular_->GetGrainSizeFraction01(),
                                  granular_->GetScanPosition01(), granular_->GetScanEnd01());

            // Button1/Button2 toggle which pair K1/K2 reach -- Start+End
            // (the range itself) or just Gap (Scan's own, K1 only; see
            // GranularEngine::SetScanGap01()'s own comment) -- same
            // toggle idiom as the Grain page's own Size+Fill/Gap+Jitter
            // pair.
            if(!granular_scanrange_target_gap_)
            {
                char start_val[8], end_val[8];
                snprintf(start_val, sizeof(start_val), "%d%%",
                          (int)(granular_->GetScanPosition01() * 100.f + 0.5f));
                snprintf(end_val, sizeof(end_val), "%d%%",
                          (int)(granular_->GetScanEnd01() * 100.f + 0.5f));
                DrawControlRow(kFooterRow1Y, false, kFooterDividerY, "Start", start_val, end_val,
                                 "End");
            }
            else
            {
                char gap_val[8];
                snprintf(gap_val, sizeof(gap_val), "%d%%",
                          (int)(granular_->GetScanGap01() * 100.f + 0.5f));
                DrawControlRow(kFooterRow1Y, false, kFooterDividerY, "Gap", gap_val, "", "");
            }
            // Marks which pair Button1 currently maps the knobs to, same
            // "*" convention as the Grain page's own toggle indicator.
            DrawControlRow(kFooterRow2Y, true, kFooterInterRowDividerY,
                             granular_scanrange_target_gap_ ? "StEnd" : "StEnd*", "", "",
                             granular_scanrange_target_gap_ ? "Gap*" : "Gap");
            break;
        }
        case GranularParamPage::Scan:
        {
            // Bounce (reverse at each edge) vs Wrap (jump back to the
            // other edge, same direction) -- Button1 toggles. Direction
            // (Forward/Reverse/Random, independent of the sweep's own
            // Bounce/Wrap heading -- see GranularEngine::
            // ResolveScanDirectionSign()'s own comment) -- Button2
            // cycles. Same "top text line shows the button-controlled
            // mode(s)" idiom as Position's own "Rhy:.. Spd:.." line.
            char scan_line1[28];
            snprintf(scan_line1, sizeof(scan_line1), "Mode:%s Dir:%s",
                      granular_->GetScanBounce() ? "Bounce" : "Wrap",
                      GranularDirectionName(granular_->GetScanDirection()));
            TomThumbDrawText(disp_, 0, 15, scan_line1, true);

            // Same waveform + anchor markers as Grain/Position -- shows
            // the Scan layer's own sweeping anchor.
            DrawGranularWaveform(granular_->GetWaveformPeaks(), granular_->GetGrainAnchor01(),
                                  granular_->GetScanAnchor01(), granular_->HasSource(),
                                  granular_->GetGrainSizeFraction01());

            char sc_val[8], fl_val[8];
            if(granular_->IsScanMuted())
                snprintf(sc_val, sizeof(sc_val), "Mute");
            else
                snprintf(sc_val, sizeof(sc_val), "%d%%",
                          (int)(granular_->GetScan01() * 100.f + 0.5f));
            snprintf(fl_val, sizeof(fl_val), "%d", granular_->GetScanFill());
            DrawControlRow(kFooterRow1Y, false, kFooterDividerY, "Scan", sc_val, fl_val, "Fill");
            // Button1 toggles Bounce/Wrap, Button2 cycles Direction (see
            // the top text line above for both current states).
            DrawControlRow(kFooterRow2Y, true, kFooterInterRowDividerY, "Bounce/Wrap", "", "",
                             "Direction");
            break;
        }
        case GranularParamPage::Tune:
        {
            int semis = granular_->GetGrainTuneSemitones();
            TomThumbDrawText(disp_, 0, 15,
                               granular_->GetGrainFollowsNote() ? "Map to note: On"
                                                                  : "Map to note: Off",
                               true);

            // Same waveform + anchor markers as Grain/Position -- this
            // page's own one knob (Tune) doesn't move an anchor, but
            // showing it anyway keeps every Grains page from looking
            // empty. Direction moved to Position's own K2.
            DrawGranularWaveform(granular_->GetWaveformPeaks(), granular_->GetGrainAnchor01(),
                                  granular_->GetScanAnchor01(), granular_->HasSource(),
                                  granular_->GetGrainSizeFraction01());

            char tune_val[8];
            snprintf(tune_val, sizeof(tune_val), "%+d", semis);
            DrawControlRow(kFooterRow1Y, false, kFooterDividerY, "Tune", tune_val, "", "");
            DrawControlRow(kFooterRow2Y, true, kFooterInterRowDividerY,
                             granular_->GetGrainFollowsNote() ? "Map:On" : "Map:Off", "", "", "");
            break;
        }
        case GranularParamPage::ADSR:
        {
            // All four stages always shown together, same idiom as Pad's
            // own merged ADSR page and Grain's Size/Fill/Gap/Scan readout.
            DrawAdsrShape(kBandTop, kBandBottom, granular_->GetAttackSeconds(),
                          granular_->GetDecaySeconds(), granular_->GetSustain01(),
                          granular_->GetReleaseSeconds());
            if(!granular_adsr_target_sr_)
            {
                char a_val[8], d_val[8];
                snprintf(a_val, sizeof(a_val), "%d%%",
                          (int)(granular_->GetAttack01() * 100.f + 0.5f));
                snprintf(d_val, sizeof(d_val), "%d%%",
                          (int)(granular_->GetDecay01() * 100.f + 0.5f));
                DrawControlRow(kFooterRow1Y, false, kFooterDividerY, "Attack", a_val, d_val,
                                 "Decay");
            }
            else
            {
                char s_val[8], r_val[8];
                snprintf(s_val, sizeof(s_val), "%d%%",
                          (int)(granular_->GetSustain01() * 100.f + 0.5f));
                snprintf(r_val, sizeof(r_val), "%d%%",
                          (int)(granular_->GetRelease01() * 100.f + 0.5f));
                DrawControlRow(kFooterRow1Y, false, kFooterDividerY, "Sustain", s_val, r_val,
                                 "Release");
            }
            DrawControlRow(kFooterRow2Y, true, kFooterInterRowDividerY,
                             granular_adsr_target_sr_ ? "AD" : "AD*", "", "",
                             granular_adsr_target_sr_ ? "SR*" : "SR");
            break;
        }
        case GranularParamPage::Filter:
        {
            // Mode name in Tom Thumb at the top (was the big font, eating
            // into the oscilloscope's own space) -- frees up real room
            // for the scope, which now gets almost the whole band instead
            // of losing 9px to a tall text line.
            char mode_line[20];
            snprintf(mode_line, sizeof(mode_line), "Mode: %s",
                      FilterModeName(granular_->GetFilterMode()));
            TomThumbDrawText(disp_, 0, 15, mode_line, true);
            DrawOscilloscope(18, kBandBottom, granular_scope_buf_, granular_scope_capacity_);

            char cutoff_val[8], res_val[8];
            snprintf(cutoff_val, sizeof(cutoff_val), "%d%%",
                      (int)(granular_->GetFilterCutoff01() * 100.f + 0.5f));
            snprintf(res_val, sizeof(res_val), "%d%%",
                      (int)(granular_->GetFilterResonance01() * 100.f + 0.5f));
            DrawControlRow(kFooterRow1Y, false, kFooterDividerY, "Cutoff", cutoff_val, res_val,
                             "Res");
            DrawControlRow(kFooterRow2Y, true, kFooterInterRowDividerY, "Cycle mode", "", "",
                             "");
            break;
        }
        case GranularParamPage::Mix:
        {
            // Same label-over-value column layout as the Grain page's
            // Size/Fill/Gap/Scan readout, plus a level bar under each
            // value -- same outline+fill technique as this project's own
            // hold-to-confirm progress bars. All three always shown
            // (same "graph shows everything, buttons only change what
            // the knobs reach" idiom as Pad's own ADSR page) -- Reverb
            // Send here is the SAME granular_->GetReverbSend01() also
            // reachable from Global:Mixer, so changing it here updates
            // there too with no extra sync needed.
            const char* mix_labels[3] = {"Grain", "Scan", "Reverb"};
            float       mix_vals01[3] = {granular_->GetGrainVolume01(),
                                          granular_->GetScanVolume01(),
                                          granular_->GetReverbSend01()};
            char        mix_values[3][8];
            for(int i = 0; i < 3; i++)
                snprintf(mix_values[i], sizeof(mix_values[i]), "%d%%",
                          (int)(mix_vals01[i] * 100.f + 0.5f));

            const int kMixLabelBaseline = 15;
            const int kMixValueBaseline = 21;
            const int kMixColWidth      = disp_->Width() / 3;
            const int kBarWidth = 34, kBarHeight = 6, kBarTop = 25;
            for(int i = 0; i < 3; i++)
            {
                int center_x = kMixColWidth * i + kMixColWidth / 2;
                int lw       = TomThumbAdvanceWidth(mix_labels[i]);
                TomThumbDrawText(disp_, center_x - lw / 2, kMixLabelBaseline, mix_labels[i],
                                  true);
                int vw = TomThumbAdvanceWidth(mix_values[i]);
                TomThumbDrawText(disp_, center_x - vw / 2, kMixValueBaseline, mix_values[i],
                                  true);

                int bar_x0 = center_x - kBarWidth / 2;
                int bar_x1 = bar_x0 + kBarWidth - 1;
                disp_->DrawRect(bar_x0, kBarTop, bar_x1, kBarTop + kBarHeight - 1, true, false);
                int fill_w
                    = (int)(Clampf(mix_vals01[i], 0.f, 1.f) * (float)(kBarWidth - 2) + 0.5f);
                if(fill_w > 0)
                    disp_->DrawRect(bar_x0 + 1, kBarTop + 1, bar_x0 + fill_w,
                                      kBarTop + kBarHeight - 2, true, true);
            }

            if(!granular_mix_target_reverb_)
                DrawControlRow(kFooterRow1Y, false, kFooterDividerY, "Grain", mix_values[0],
                                 mix_values[1], "Scan");
            else
                DrawControlRow(kFooterRow1Y, false, kFooterDividerY, "Reverb", mix_values[2],
                                 "", "");
            DrawControlRow(kFooterRow2Y, true, kFooterInterRowDividerY,
                             granular_mix_target_reverb_ ? "Grain/Scan" : "Grain/Scan*", "", "",
                             granular_mix_target_reverb_ ? "Reverb*" : "Reverb");
            break;
        }
        case GranularParamPage::FX:
        {
            // Full reverb/delay control from within Grains itself -- Mix's
            // own Grain/Scan/Reverb Send above are untouched, this is an
            // additional page (see GranularParamPage::FX's own comment).
            if(!granular_fx_target_delay_)
            {
                char send_val[8], size_val[8];
                snprintf(send_val, sizeof(send_val), "%d%%",
                          (int)(granular_->GetReverbSend01() * 100.f + 0.5f));
                snprintf(size_val, sizeof(size_val), "%d%%",
                          (int)(reverb_size01_ * 100.f + 0.5f));
                disp_->SetCursor(0, 20);
                WriteUpper("Reverb");
                DrawControlRow(kFooterRow1Y, false, kFooterDividerY, "Send", send_val, size_val,
                                 "Size");
            }
            else
            {
                char send_val[8], time_val[8];
                snprintf(send_val, sizeof(send_val), "%d%%",
                          (int)(granular_->GetDelaySend01() * 100.f + 0.5f));
                snprintf(time_val, sizeof(time_val), "%d%%",
                          (int)(delay_time01_ * 100.f + 0.5f));
                disp_->SetCursor(0, 20);
                WriteUpper("Delay");
                DrawControlRow(kFooterRow1Y, false, kFooterDividerY, "Send", send_val, time_val,
                                 "Time");
            }
            DrawControlRow(kFooterRow2Y, true, kFooterInterRowDividerY,
                             granular_fx_target_delay_ ? "Reverb" : "Reverb*", "", "",
                             granular_fx_target_delay_ ? "Delay*" : "Delay");
            break;
        }
        case GranularParamPage::MIDI:
        {
            char chan_val[8];
            if(granular_midi_channel_ < 0)
                snprintf(chan_val, sizeof(chan_val), "Omni");
            else
                snprintf(chan_val, sizeof(chan_val), "%d", granular_midi_channel_ + 1);
            // No body text of its own needed (title already says "MIDI").
            DrawControlRow(kFooterRow1Y, false, kFooterDividerY, "Channel", chan_val, "", "");
            DrawControlRow(kFooterRow2Y, true, kFooterInterRowDividerY, "", "", "", "");
            break;
        }
        case GranularParamPage::Capture:
        {
            // Hold-to-confirm progress bar for the two fire-once gestures
            // (From Layer, Import) -- same pattern as Preset's own
            // "Hold: Load..." bar. Direct Record is deliberately excluded:
            // holding Button2 there IS the recording itself (see the
            // "Recording %" bar further down), not a confirm-then-fire
            // action.
            if(pod_->button2.Pressed()
               && (granular_capture_source_ >= 0 || granular_capture_source_ == -2))
            {
                float held = pod_->button2.TimeHeldMs();
                int   w    = (int)(Clampf(held / 800.f, 0.f, 1.f) * (disp_->Width() - 2));
                const char* hold_msg
                    = granular_capture_source_ == -2 ? "Hold: Import..." : "Hold: Capture...";
                TomThumbDrawText(disp_, 0, 15, hold_msg, true);
                disp_->DrawRect(0, 20, disp_->Width() - 1, 24, true, false);
                if(w > 0)
                    disp_->DrawRect(1, 21, w, 23, true, true);
                DrawControlRow(kFooterRow1Y, false, kFooterDividerY, "", "", "", "");
                DrawControlRow(kFooterRow2Y, true, kFooterInterRowDividerY, "", "", "", "");
                break;
            }

            char src_line[24];
            if(granular_capture_source_ >= 0)
                snprintf(src_line, sizeof(src_line), "Source: Layer %d",
                          granular_capture_source_ + 1);
            else if(granular_capture_source_ == -2)
                snprintf(src_line, sizeof(src_line), "Source: Import");
            else
                snprintf(src_line, sizeof(src_line), "Source: Direct Record");
            TomThumbDrawText(disp_, 0, 15, src_line, true);

            if(granular_capture_source_ == -2 && granular_import_files_dirty_)
                RefreshGranularImportFiles();

            bool recording = granular_capture_source_ == -1 && granular_capturing_
                              && *granular_capturing_;
            if(granular_capture_source_ == -2)
            {
                // Browse the list of .wav files found in IMPORT/ --
                // same "Load: <name>"-style live feedback as Preset's
                // own browsing, status takes priority when there is one.
                if(granular_capture_status_[0] != '\0')
                {
                    TomThumbDrawText(disp_, 0, 22, granular_capture_status_, true);
                }
                else if(granular_import_file_count_ > 0)
                {
                    // Scrollable list, not just the single selected name --
                    // up to kVisibleRows entries at once (baselines 22, 28,
                    // 34, 40 -- 6px TomThumb rows, fits between the "Source:"
                    // line above and the footer divider below), with the
                    // window centered on the cursor and clamped to the
                    // list's own ends so it doesn't scroll past them.
                    constexpr int kVisibleRows = 4;
                    int           count        = granular_import_file_count_;
                    int           start        = granular_import_cursor_ - kVisibleRows / 2;
                    if(start > count - kVisibleRows)
                        start = count - kVisibleRows;
                    if(start < 0)
                        start = 0;
                    int rows = count < kVisibleRows ? count : kVisibleRows;
                    for(int row = 0; row < rows; row++)
                    {
                        int  idx      = start + row;
                        bool selected = idx == granular_import_cursor_;
                        char line[64];
                        snprintf(line, sizeof(line), "%c%s", selected ? '>' : ' ',
                                  granular_import_names_[idx]);
                        // Truncate from the end until it fits -- a long
                        // sample filename (e.g. exported from a DAW)
                        // otherwise runs straight past the 128px display
                        // width.
                        while(line[0] != '\0' && TomThumbAdvanceWidth(line) > disp_->Width())
                            line[strlen(line) - 1] = '\0';
                        TomThumbDrawText(disp_, 0, 22 + row * 6, line, true);
                    }
                }
                else
                {
                    TomThumbDrawText(disp_, 0, 22, "No .wav files in IMPORT/", true);
                }
            }
            else if(recording)
            {
                // Holding Button2 IS the recording -- this bar is both
                // progress feedback and the "how much of the 5s cap is
                // left" readout, live-updating from the ISR's own
                // write_pos as it fills.
                float frac = granular_capture_capacity_ > 0
                                 ? (float)*granular_capture_write_pos_
                                       / (float)granular_capture_capacity_
                                 : 0.f;
                char rec_line[20];
                snprintf(rec_line, sizeof(rec_line), "Recording %d%%",
                          (int)(frac * 100.f + 0.5f));
                TomThumbDrawText(disp_, 0, 22, rec_line, true);

                const int kBarX0 = 0, kBarX1 = disp_->Width() - 1, kBarY0 = 28, kBarY1 = 36;
                disp_->DrawRect(kBarX0, kBarY0, kBarX1, kBarY1, true, false);
                int fill_w
                    = (int)(Clampf(frac, 0.f, 1.f) * (float)(kBarX1 - kBarX0 - 1) + 0.5f);
                if(fill_w > 0)
                    disp_->DrawRect(kBarX0 + 1, kBarY0 + 1, kBarX0 + fill_w, kBarY1 - 1, true,
                                      true);
            }
            else
            {
                if(granular_capture_status_[0] != '\0')
                    TomThumbDrawText(disp_, 0, 22, granular_capture_status_, true);
                DrawGranularWaveform(granular_->GetWaveformPeaks(), granular_->GetGrainAnchor01(),
                                      granular_->GetScanAnchor01(), granular_->HasSource(),
                                      granular_->GetGrainSizeFraction01());
            }

            const char* hold_label = "Hold=Record";
            if(granular_capture_source_ >= 0)
                hold_label = "Hold=Capture";
            else if(granular_capture_source_ == -2)
                hold_label = "Hold=Import";
            DrawControlRow(kFooterRow1Y, false, kFooterDividerY, "", "", "", "");
            DrawControlRow(kFooterRow2Y, true, kFooterInterRowDividerY, "Cycle Source", "", "",
                             hold_label);
            break;
        }
        case GranularParamPage::Trim:
        {
            char line[24];
            snprintf(line, sizeof(line), "Start%d%% End%d%%",
                      (int)(granular_trim_start01_ * 100.f + 0.5f),
                      (int)(granular_trim_end01_ * 100.f + 0.5f));
            TomThumbDrawText(disp_, 0, 15, line, true);

            // Reuses DrawGranularWaveform()'s two-marker drawing as-is --
            // here the markers are the trim start/end points rather than
            // Grain/Scan anchors, over the FULL untrimmed capture (see
            // granular_trim_full_peaks_'s own comment), not whatever
            // sub-range is currently active.
            DrawGranularWaveform(granular_trim_full_peaks_, granular_trim_start01_,
                                  granular_trim_end01_, granular_capture_full_len_ > 0);

            char s_val[8], e_val[8];
            snprintf(s_val, sizeof(s_val), "%d%%", (int)(granular_trim_start01_ * 100.f + 0.5f));
            snprintf(e_val, sizeof(e_val), "%d%%", (int)(granular_trim_end01_ * 100.f + 0.5f));
            DrawControlRow(kFooterRow1Y, false, kFooterDividerY, "Start", s_val, e_val, "End");
            DrawControlRow(kFooterRow2Y, true, kFooterInterRowDividerY, "Play", "", "", "");
            break;
        }
        case GranularParamPage::Preset:
        {
            if(!PerformanceStore::IsCardPresent())
            {
                disp_->SetCursor(0, 20);
                WriteUpper("No card");
                DrawControlRow(kFooterRow1Y, false, kFooterDividerY, "", "", "", "");
                DrawControlRow(kFooterRow2Y, true, kFooterInterRowDividerY, "Retry", "", "", "");
                break;
            }
            if(granular_preset_slots_dirty_)
                RefreshGranularPresetSlots();

            bool choosing_save = save_load_mode_ == SaveLoadMode::ChoosingSave;
            bool browsing_load = save_load_mode_ == SaveLoadMode::BrowsingLoad;
            bool load_chooser  = browsing_load && !load_browsing_files_;
            bool can_hold_load = browsing_load && (load_browsing_files_ || load_new_selected_);

            // Body text uses the same bigger Font_6x8/WriteUpper
            // convention as Global:File (not TomThumb, unlike the rest of
            // this screen's pages) -- same reasoning: this is the one
            // page where matching Global:File's exact look matters more
            // than matching Grains' own other pages.
            //
            // Button2 confirms whichever of Save/Load is active (Button1
            // is always "Back" (or "Select" on the Load chooser) once
            // inside either state -- see OnButton1Short()/
            // HandleButton2()). The actual SD transfer (once the hold
            // fires) takes over the WHOLE display via
            // Ui::OnSaveLoadProgress() instead, since it can run long
            // enough (up to ~1.9MB of audio) to need its own feedback,
            // unlike Pad's instant tiny-struct load.
            if(pod_->button2.Pressed() && (choosing_save || can_hold_load))
            {
                float       held     = pod_->button2.TimeHeldMs();
                int         w        = (int)(Clampf(held / 800.f, 0.f, 1.f) * (disp_->Width() - 2));
                const char* hold_msg = choosing_save ? "Hold: Save..." : "Hold: Load...";
                disp_->SetCursor(0, 20);
                WriteUpper(hold_msg);
                disp_->DrawRect(0, 30, disp_->Width() - 1, 34, true, false);
                if(w > 0)
                    disp_->DrawRect(1, 31, w, 33, true, true);
                DrawControlRow(kFooterRow1Y, false, kFooterDividerY, "", "", "", "");
                DrawControlRow(kFooterRow2Y, true, kFooterInterRowDividerY, "", "", "", "");
                break;
            }

            if(choosing_save)
            {
                bool can_overwrite = granular_loaded_preset_slot_ > 0;
                char line1[24], line2[16];
                snprintf(line1, sizeof(line1), "%c Overwrite%s", !save_as_new_ ? '>' : ' ',
                          can_overwrite ? "" : " (n/a)");
                snprintf(line2, sizeof(line2), "%c Save New", save_as_new_ ? '>' : ' ');
                disp_->SetCursor(0, 16);
                WriteUpper(line1);
                disp_->SetCursor(0, 28);
                WriteUpper(line2);
            }
            else if(load_chooser)
            {
                // Same shape as ChoosingSave's own Overwrite/Save New --
                // Files only a real option once something's actually
                // saved, same "can't select what doesn't exist" reasoning
                // as Overwrite's own.
                char line1[24], line2[16];
                snprintf(line1, sizeof(line1), "%c Files%s", !load_new_selected_ ? '>' : ' ',
                          granular_preset_user_slot_count_ > 0 ? "" : " (n/a)");
                snprintf(line2, sizeof(line2), "%c Load New", load_new_selected_ ? '>' : ' ');
                disp_->SetCursor(0, 16);
                WriteUpper(line1);
                disp_->SetCursor(0, 28);
                WriteUpper(line2);
            }
            else if(browsing_load)
            {
                // Drilled into the numbered list (load_browsing_files_ ==
                // true).
                // "GRAINS" matches Global:SdMgmt's own short code for
                // this category (SdMgmtFolderShortLabel()) -- one
                // shared vocabulary everywhere a bare slot number is
                // shown, not a second one invented here.
                char line2[24];
                snprintf(line2, sizeof(line2), "Load: GRAINS%d",
                          granular_preset_user_slots_[granular_preset_cursor_]);
                disp_->SetCursor(0, 20);
                WriteUpper(line2);
            }
            else
            {
                char line1[24];
                if(granular_loaded_preset_slot_ <= 0)
                    snprintf(line1, sizeof(line1), "Now: (custom)");
                else
                    snprintf(line1, sizeof(line1), "Now: GRAINS%d", granular_loaded_preset_slot_);
                disp_->SetCursor(0, 20);
                WriteUpper(line1);
                if(granular_preset_status_[0] != '\0')
                {
                    disp_->SetCursor(0, 32);
                    WriteUpper(granular_preset_status_);
                }
            }

            {
                const char* b1_label;
                if(save_load_mode_ == SaveLoadMode::Idle)
                    b1_label = "Save";
                else if(load_chooser && !load_new_selected_)
                    b1_label = "Select";
                else
                    b1_label = "Back";
                const char* b2_label;
                if(save_load_mode_ == SaveLoadMode::Idle)
                    b2_label = "Load";
                else if(choosing_save)
                    b2_label = "Hold=Save";
                else if(can_hold_load)
                    b2_label = "Hold=Load";
                else
                    b2_label = "";

                DrawControlRow(kFooterRow1Y, false, kFooterDividerY,
                                 (choosing_save || browsing_load) ? "Scroll" : "", "", "", "");
                DrawControlRow(kFooterRow2Y, true, kFooterInterRowDividerY, b1_label, "", "",
                                 b2_label);
            }
            break;
        }
        default: break;
    }
}

const char* Ui::MixerChannelName(int ch) const
{
    switch(ch)
    {
        case 0: return "L1";
        case 1: return "L2";
        case 2: return "L3";
        case 3: return "L4";
        case 4: return "GR";
        case 5: return "DXD";
        case 6: return "ESB";
        case 7: return "SMP";
        case 8: return "BYP";
        case 9: return "MAS";
        default: return "?";
    }
}

float Ui::MixerGetVolume01(int ch) const
{
    switch(ch)
    {
        case 0: case 1: case 2: case 3: return layers_[ch].GetVolume01();
        case 4: return granular_ ? granular_->GetOutputLevel01() : 0.f;
        case 5: return dexed_ ? dexed_->GetOutputLevel01() : 0.f;
        case 6: return ensemble_ ? ensemble_->GetOutputLevel01() : 0.f;
        case 7: return sample_synth_ ? sample_synth_->GetOutputLevel01() : 0.f;
        case 8: return bypass_mix_volume01_;
        case 9: return master_volume01_;
        default: return 0.f;
    }
}

float Ui::MixerGetPan01(int ch) const
{
    switch(ch)
    {
        case 0: case 1: case 2: case 3: return layers_[ch].GetPan01();
        case 4: return granular_ ? granular_->GetPan01() : 0.5f;
        case 5: return dexed_ ? dexed_->GetPan01() : 0.5f;
        case 6: return ensemble_ ? ensemble_->GetPan01() : 0.5f;
        case 7: return sample_synth_ ? sample_synth_->GetPan01() : 0.5f;
        case 8: return bypass_pan01_;
        default: return 0.5f; // Master has none
    }
}

float Ui::MixerGetSend01(int ch) const
{
    switch(ch)
    {
        case 0: case 1: case 2: case 3: return layers_[ch].GetReverbSend01();
        case 4: return granular_ ? granular_->GetReverbSend01() : 0.f;
        case 5: return dexed_ ? dexed_->GetReverbSend01() : 0.f;
        case 6: return ensemble_ ? ensemble_->GetReverbSend01() : 0.f;
        case 7: return sample_synth_ ? sample_synth_->GetReverbSend01() : 0.f;
        case 8: return bypass_reverb_send01_;
        default: return 0.f; // Master uses Reverb Size instead (see reverb_size01_)
    }
}

void Ui::MixerSetVolume01(int ch, float v01)
{
    switch(ch)
    {
        case 0: case 1: case 2: case 3: layers_[ch].SetVolume01(v01); break;
        case 4: if(granular_) granular_->SetOutputLevel01(v01); break;
        case 5: if(dexed_) dexed_->SetOutputLevel01(v01); break;
        case 6: if(ensemble_) ensemble_->SetOutputLevel01(v01); break;
        case 7: if(sample_synth_) sample_synth_->SetOutputLevel01(v01); break;
        case 8: SetBypassMixVolume01(v01); break;
        case 9:
            master_volume01_ = Clampf(v01, 0.f, 1.f);
            master_volume_   = powf(master_volume01_, 2.5f) * 1.43f;
            if(master_volume_ < 0.f)
                master_volume_ = 0.f;
            break;
        default: break;
    }
}

void Ui::MixerSetPan01(int ch, float v01)
{
    switch(ch)
    {
        case 0: case 1: case 2: case 3: layers_[ch].SetPan01(v01); break;
        case 4: if(granular_) granular_->SetPan01(v01); break;
        case 5: if(dexed_) dexed_->SetPan01(v01); break;
        case 6: if(ensemble_) ensemble_->SetPan01(v01); break;
        case 7: if(sample_synth_) sample_synth_->SetPan01(v01); break;
        case 8: SetBypassPan01(v01); break;
        default: break; // Master has no Pan -- no-op
    }
}

void Ui::MixerSetSend01(int ch, float v01)
{
    switch(ch)
    {
        case 0: case 1: case 2: case 3: layers_[ch].SetReverbSend01(v01); break;
        case 4: if(granular_) granular_->SetReverbSend01(v01); break;
        case 5: if(dexed_) dexed_->SetReverbSend01(v01); break;
        case 6: if(ensemble_) ensemble_->SetReverbSend01(v01); break;
        case 7: if(sample_synth_) sample_synth_->SetReverbSend01(v01); break;
        case 8: bypass_reverb_send01_ = Clampf(v01, 0.f, 1.f); break;
        default: break; // Master uses Reverb Size instead, set directly in ApplyKnobs()
    }
}

bool Ui::MixerChannelHasPan(int ch) const
{
    // Only Master lacks a Pan concept now -- Dexed got a real Pan
    // control after a user report that its live output, having no pan
    // stage at all, reached the mix disproportionately loud next to
    // every other source's own center-pan-attenuated signal.
    return ch != kNumMixerChannels - 1;
}

float Ui::MixerGetLevel01(int ch) const
{
    switch(ch)
    {
        case 0: case 1: case 2: case 3: return layers_[ch].GetLevel();
        // Grains/Dexed read their own PRE-limiter level now (same
        // reasoning as MixerIsClipping()'s own comment) -- the
        // post-chain g_grains_level/g_dexed_level globals are still
        // computed (main.cpp's scope/meter loops need to run either
        // way) but no longer drawn, since they looked permanently
        // quiet: post-tanhf(), post-output_level_, and post-pan all
        // stacked on top of each other between "the engine is working
        // hard" and what that bar would have shown.
        case 4: return granular_ ? granular_->GetPreLimiterLevel01() : 0.f;
        case 5: return dexed_ ? dexed_->GetPreLimiterLevel01() : 0.f;
        case 6: return ensemble_ ? ensemble_->GetPreLimiterLevel01() : 0.f;
        case 7: return sample_synth_ ? sample_synth_->GetPreLimiterLevel01() : 0.f;
        case 8: return g_bypass_level;
        case 9: return g_master_level;
        default: return 0.f;
    }
}

bool Ui::MixerIsClipping(int ch) const
{
    switch(ch)
    {
        case 0: case 1: case 2: case 3: return layers_[ch].IsClipping();
        // Grains/Dexed each own their own PRE-limiter clip check now
        // (IsLimiting()) instead of the post-chain g_grains_clip_hold/
        // g_dexed_clip_hold globals -- see those methods' own comments
        // for why a post-tanhf() check could never usefully fire here:
        // tanhf() bounds its output to (-1,1) no matter how hard it's
        // being driven, so the only way to detect "the limiter is
        // actually compressing hard" is to check before it, inside the
        // engine itself. Bypass/Master have no such internal limiter,
        // so those two keep using the plain post-chain check.
        case 4: return granular_ && granular_->IsLimiting();
        case 5: return dexed_ && dexed_->IsLimiting();
        case 6: return ensemble_ && ensemble_->IsLimiting();
        case 7: return sample_synth_ && sample_synth_->IsLimiting();
        case 8: return g_bypass_clip_hold > 0.f;
        case 9: return g_master_clip_hold > 0.f;
        default: return false;
    }
}

// Vertical fader-style bar -- (x0,y0) is the box's top-left corner, fills
// from the BOTTOM up (unlike every other bar in this project, which fills
// left-to-right -- see the Mixer design discussion this was built from
// for why a vertical fader reads better for a per-channel level here).
void Ui::DrawMixerVBar(int x0, int y0, int w, int h, float v01, bool clip_lit)
{
    int x1 = x0 + w - 1;
    int y1 = y0 + h - 1;
    disp_->DrawRect(x0, y0, x1, y1, true, false);
    int fill_h = (int)(Clampf(v01, 0.f, 1.f) * (float)(h - 2) + 0.5f);
    if(fill_h > 0)
        disp_->DrawRect(x0 + 1, y1 - fill_h, x1 - 1, y1 - 1, true, true);
    // Clip-indicator latch, drawn as a filled cap right at the top of
    // the bar's own outline rather than a separate element -- see
    // DrawMixerVBar()'s own doc comment (ui.h) for why: this costs no
    // extra screen space on either the cramped Overview grid or the
    // Detail page. Drawn last so it's never hidden under the fill.
    if(clip_lit)
        disp_->DrawRect(x0 + 1, y0 + 1, x1 - 1, y0 + 2, true, true);
}

void Ui::DrawMixerOverviewGrid(int top_y)
{
    // All 8 names across in one row, name on top, a real vertical bar
    // below -- but unlike Screen::Mixer's own Detail page, this bar
    // shows live Peak level, not the Volume fader's own setting (see
    // MixerGetLevel01()'s own comment): this grid is meant to answer
    // "what's loud/clipping right now", and a fader-position readout
    // would just duplicate what the Detail page's Vol bar already
    // shows. The clip-indicator cap (same DrawMixerVBar() mechanism)
    // makes this a real "did anything clip" glance too, not just a
    // relative-loudness readout. No selection box here -- unlike
    // Screen::Mixer's own Detail page, there's no meaningful "currently
    // selected" channel on this at-a-glance summary; mixer_position_ is
    // just wherever Screen::Mixer was last left, not something being
    // actively chosen from here.
    // Bars stop at y=37, not the usual y=45 footer-divider margin -- this
    // grid is also used above the "PUSH ENC TO ENTER" caption on
    // Global:Mixer (see DrawGlobalScreen()), which needs its own clear
    // strip beneath.
    // Integer division of Width/kNumMixerChannels floors, so the columns
    // never exactly fill the screen (e.g. 128/10 = 12, using only 120 of
    // 128px) -- left as a flush-left block, that leftover always lands
    // as dead space on the right, making the row look unevenly spaced.
    // kLeftMargin centers the whole block instead, splitting the
    // leftover evenly on both sides.
    const int kColW       = disp_->Width() / kNumMixerChannels;
    const int kLeftMargin = (disp_->Width() - kColW * kNumMixerChannels) / 2;
    const int kBarW  = 8;
    const int kBarY0 = top_y + 4;
    const int kBarH  = 37 - kBarY0;
    for(int ch = 0; ch < kNumMixerChannels; ch++)
    {
        int col_x    = kLeftMargin + kColW * ch;
        int center_x = col_x + kColW / 2;

        const char* name = MixerChannelName(ch);
        int         nw   = TomThumbAdvanceWidth(name);
        TomThumbDrawText(disp_, center_x - nw / 2, top_y + 1, name, true);

        DrawMixerVBar(center_x - kBarW / 2, kBarY0, kBarW, kBarH, MixerGetLevel01(ch),
                       MixerIsClipping(ch));
    }
}

void Ui::DrawDexedScreen()
{
    if(!dexed_)
        return;

    const char* page_name = "Algo";
    switch(dexed_param_page_)
    {
        case DexedParamPage::Algo: page_name = "Algo"; break;
        case DexedParamPage::Feedback: page_name = "Feedback"; break;
        case DexedParamPage::Vibrato: page_name = "MOD"; break;
        case DexedParamPage::Brightness: page_name = "Bright"; break;
        case DexedParamPage::EnvSpeed: page_name = "EnvSpd"; break;
        case DexedParamPage::Filter: page_name = "Filter"; break;
        case DexedParamPage::Mix: page_name = "Mix"; break;
        case DexedParamPage::FX: page_name = "DX-FX"; break;
        case DexedParamPage::Fold: page_name = "DX-FOLD"; break;
        case DexedParamPage::MIDI: page_name = "MIDI"; break;
        case DexedParamPage::Advanced: page_name = "Advanced"; break;
        case DexedParamPage::Preset: page_name = "Preset"; break;
        default: break;
    }
    char title[24];
    snprintf(title, sizeof(title), "Dexed:%s", page_name);
    disp_->SetCursor(0, 0);
    WriteUpper(title);
    DrawBeatIndicator(disp_->Width() - 41, 0, 3);
    disp_->DrawLine(0, 9, disp_->Width() - 1, 9, true);

    switch(dexed_param_page_)
    {
        case DexedParamPage::Algo:
        {
            int algo0 = dexed_->GetPatchByte(134);
            DrawAlgorithmDiagram(algo0);
            char algo_val[8];
            snprintf(algo_val, sizeof(algo_val), "%d", algo0 + 1);
            DrawControlRow(kFooterRow1Y, false, kFooterDividerY, "Algo", algo_val, "", "");
            DrawControlRow(
                kFooterRow2Y, true, kFooterInterRowDividerY, "Cycle -", "", "", "Cycle +");
            break;
        }
        case DexedParamPage::Feedback:
        {
            char val[8];
            snprintf(val, sizeof(val), "%d", dexed_->GetPatchByte(135));
            // No body text of its own needed (the title already says
            // "Feedback") -- the freed-up space goes to Dexed's own live
            // scope instead, same idiom as GranularParamPage::Filter.
            DrawOscilloscope(12, kFooterDividerY - 2, dexed_scope_buf_, dexed_scope_capacity_);
            DrawControlRow(kFooterRow1Y, false, kFooterDividerY, "Amt", val, "", "");
            DrawControlRow(kFooterRow2Y, true, kFooterInterRowDividerY, "", "", "", "");
            break;
        }
        case DexedParamPage::Vibrato:
        {
            disp_->SetCursor(0, 20);
            WriteUpper(dexed_->GetModWheelTargetName());
            // Button1 cycles the real DX7 mod-wheel routing (Pitch/Amp/EG
            // Bias, see DexedSynth::ModWheelTarget's own comment).
            // Button2 toggles the knobs below between the patch's own
            // baked-in automatic vibrato (Speed/Depth) and the global
            // Pitch Mod Sensitivity byte that gates whether Pitch (and
            // the Depth knob itself) can do anything audible at all --
            // added after a real report that every mod-wheel target was
            // silent, traced to this byte never being exposed.
            if(!dexed_vibrato_target_sens_)
            {
                char speed_val[8], depth_val[8];
                snprintf(speed_val, sizeof(speed_val), "%d", dexed_->GetPatchByte(137));
                snprintf(depth_val, sizeof(depth_val), "%d", dexed_->GetPatchByte(139));
                DrawControlRow(kFooterRow1Y, false, kFooterDividerY, "Speed", speed_val,
                                 depth_val, "Depth");
            }
            else
            {
                char sens_val[8];
                snprintf(sens_val, sizeof(sens_val), "%d", dexed_->GetPatchByte(143) & 7);
                DrawControlRow(kFooterRow1Y, false, kFooterDividerY, "Sens", sens_val, "", "");
            }
            DrawControlRow(kFooterRow2Y, true, kFooterInterRowDividerY,
                             dexed_vibrato_target_sens_ ? "Wheel Tgt" : "Wheel Tgt*", "", "",
                             dexed_vibrato_target_sens_ ? "Speed/Sens*" : "Speed/Sens");
            break;
        }
        case DexedParamPage::Brightness:
        {
            char val[8];
            snprintf(val, sizeof(val), "%d%%", (int)(dexed_->GetBrightness01() * 100.f + 0.5f));
            // The redundant big "Brightness" repeat of the title is gone --
            // the real, non-redundant hint (what 50% means) moves to Tom
            // Thumb, same space-freeing idiom as GranularParamPage::Filter,
            // and the scope takes the rest.
            TomThumbDrawText(disp_, 0, 15, "50% = as saved", true);
            DrawOscilloscope(18, kFooterDividerY - 2, dexed_scope_buf_, dexed_scope_capacity_);
            DrawControlRow(kFooterRow1Y, false, kFooterDividerY, "Amt", val, "", "");
            DrawControlRow(kFooterRow2Y, true, kFooterInterRowDividerY, "", "", "", "");
            break;
        }
        case DexedParamPage::EnvSpeed:
        {
            char val[8];
            snprintf(val, sizeof(val), "%d%%", (int)(dexed_->GetEnvSpeed01() * 100.f + 0.5f));
            TomThumbDrawText(disp_, 0, 15, "50% = as saved", true);
            DrawOscilloscope(18, kFooterDividerY - 2, dexed_scope_buf_, dexed_scope_capacity_);
            DrawControlRow(kFooterRow1Y, false, kFooterDividerY, "Amt", val, "", "");
            DrawControlRow(kFooterRow2Y, true, kFooterInterRowDividerY, "", "", "", "");
            break;
        }
        case DexedParamPage::Filter:
        {
            // Mode name in Tom Thumb (was the big font) -- frees up real
            // room for the scope, same treatment as
            // GranularParamPage::Filter.
            char mode_line[20];
            snprintf(mode_line, sizeof(mode_line), "Mode: %s",
                      FilterModeName(dexed_->GetFilterMode()));
            TomThumbDrawText(disp_, 0, 15, mode_line, true);
            DrawOscilloscope(18, kFooterDividerY - 2, dexed_scope_buf_, dexed_scope_capacity_);

            char cutoff_val[8], res_val[8];
            snprintf(cutoff_val, sizeof(cutoff_val), "%d%%",
                      (int)(dexed_->GetFilterCutoff01() * 100.f + 0.5f));
            snprintf(res_val, sizeof(res_val), "%d%%",
                      (int)(dexed_->GetFilterResonance01() * 100.f + 0.5f));
            DrawControlRow(kFooterRow1Y, false, kFooterDividerY, "Cutoff", cutoff_val, res_val,
                             "Res");
            DrawControlRow(kFooterRow2Y, true, kFooterInterRowDividerY, "Cycle mode", "", "", "");
            break;
        }
        case DexedParamPage::Mix:
        {
            char send_val[8], out_val[8];
            snprintf(send_val, sizeof(send_val), "%d%%",
                      (int)(dexed_->GetReverbSend01() * 100.f + 0.5f));
            snprintf(out_val, sizeof(out_val), "%d%%",
                      (int)(dexed_->GetOutputLevel01() * 100.f + 0.5f));
            // No body text of its own needed (title already says "Mix") --
            // Level's effect is directly visible on the scope; Send isn't
            // (it feeds the shared reverb bus, not Dexed's own dry output
            // this scope taps -- same limitation GranularParamPage::Filter's
            // own scope already has for its own Reverb Send knob).
            DrawOscilloscope(12, kFooterDividerY - 2, dexed_scope_buf_, dexed_scope_capacity_);
            DrawControlRow(kFooterRow1Y, false, kFooterDividerY, "Rev", send_val, out_val,
                             "Level");
            DrawControlRow(kFooterRow2Y, true, kFooterInterRowDividerY, "", "", "", "");
            break;
        }
        case DexedParamPage::FX:
        {
            // Full reverb/delay control from within Dexed itself -- Mix's
            // own Reverb Send/Output Level above are untouched, this is
            // an additional page (see DexedParamPage::FX's own comment).
            if(!dexed_fx_target_delay_)
            {
                char send_val[8], size_val[8];
                snprintf(send_val, sizeof(send_val), "%d%%",
                          (int)(dexed_->GetReverbSend01() * 100.f + 0.5f));
                snprintf(size_val, sizeof(size_val), "%d%%",
                          (int)(reverb_size01_ * 100.f + 0.5f));
                TomThumbDrawText(disp_, 0, 15, "Reverb", true);
                DrawControlRow(kFooterRow1Y, false, kFooterDividerY, "Send", send_val, size_val,
                                 "Size");
            }
            else
            {
                char send_val[8], time_val[8];
                snprintf(send_val, sizeof(send_val), "%d%%",
                          (int)(dexed_->GetDelaySend01() * 100.f + 0.5f));
                snprintf(time_val, sizeof(time_val), "%d%%",
                          (int)(delay_time01_ * 100.f + 0.5f));
                TomThumbDrawText(disp_, 0, 15, "Delay", true);
                DrawControlRow(kFooterRow1Y, false, kFooterDividerY, "Send", send_val, time_val,
                                 "Time");
            }
            DrawOscilloscope(18, kFooterDividerY - 2, dexed_scope_buf_, dexed_scope_capacity_);
            DrawControlRow(kFooterRow2Y, true, kFooterInterRowDividerY,
                             dexed_fx_target_delay_ ? "Reverb" : "Reverb*", "", "",
                             dexed_fx_target_delay_ ? "Delay*" : "Delay");
            break;
        }
        case DexedParamPage::Fold:
        {
            // No sub-mode, unlike FX above -- just Amount + Trim, same
            // "no body text, scope takes the rest" idiom as Mix/MIDI.
            char amt_val[8], trim_val[8];
            snprintf(amt_val, sizeof(amt_val), "%d%%",
                      (int)(dexed_->GetFoldAmount01() * 100.f + 0.5f));
            snprintf(trim_val, sizeof(trim_val), "%d%%",
                      (int)(dexed_->GetFoldTrim01() * 100.f + 0.5f));
            DrawOscilloscope(12, kFooterDividerY - 2, dexed_scope_buf_, dexed_scope_capacity_);
            DrawControlRow(kFooterRow1Y, false, kFooterDividerY, "Amt", amt_val, trim_val,
                             "Trim");
            DrawControlRow(kFooterRow2Y, true, kFooterInterRowDividerY, "", "", "", "");
            break;
        }
        case DexedParamPage::MIDI:
        {
            char chan_val[8];
            if(dexed_midi_channel_ < 0)
                snprintf(chan_val, sizeof(chan_val), "Omni");
            else
                snprintf(chan_val, sizeof(chan_val), "%d", dexed_midi_channel_ + 1);
            // No body text of its own needed (title already says "MIDI")
            // -- same idiom as Feedback/Mix above.
            DrawOscilloscope(12, kFooterDividerY - 2, dexed_scope_buf_, dexed_scope_capacity_);
            DrawControlRow(kFooterRow1Y, false, kFooterDividerY, "Channel", chan_val, "", "");
            DrawControlRow(kFooterRow2Y, true, kFooterInterRowDividerY, "", "", "", "");
            break;
        }
        case DexedParamPage::Advanced:
        {
            // Entry point into Screen::DexedOperator -- same "Click to
            // open..." idiom as GlobalPage::Granular/Dexed's own entry
            // pages, just reached by a click on this page instead of a
            // Global one.
            disp_->SetCursor(0, 20);
            WriteUpper("Per-operator editor");
            disp_->SetCursor(0, 30);
            WriteUpper("Click to open");
            DrawControlRow(kFooterRow1Y, false, kFooterDividerY, "", "", "", "");
            DrawControlRow(kFooterRow2Y, true, kFooterInterRowDividerY, "", "", "", "");
            break;
        }
        case DexedParamPage::Preset:
        {
            if(!PerformanceStore::IsCardPresent())
            {
                disp_->SetCursor(0, 20);
                WriteUpper("No card");
                DrawControlRow(kFooterRow1Y, false, kFooterDividerY, "", "", "", "");
                DrawControlRow(kFooterRow2Y, true, kFooterInterRowDividerY, "Retry", "", "", "");
                break;
            }
            if(dexed_preset_slots_dirty_)
                RefreshDexedPresetSlots();

            bool choosing_save    = save_load_mode_ == SaveLoadMode::ChoosingSave;
            bool browsing_load    = save_load_mode_ == SaveLoadMode::BrowsingLoad;
            bool browsing_import  = browsing_load && dexed_import_browsing_;
            bool load_chooser     = browsing_load && !load_browsing_files_ && !dexed_import_browsing_;
            bool browsing_group
                = browsing_load && load_browsing_files_ && !dexed_files_group_open_;
            bool browsing_folders = browsing_load && load_browsing_files_ && dexed_files_group_open_
                                      && dexed_files_group_ != DexedFilesGroup::User
                                      && !dexed_preset_folder_open_;
            if(browsing_import && dexed_import_files_dirty_)
                RefreshDexedImportFiles();
            // A group/folder name isn't a preset -- nothing to
            // confirm-load until "New" is picked at the top chooser, a
            // folder is actually open onto a real preset
            // (Roms/Dexed/Imports), the User group is open (it has no
            // folder level of its own, see DexedFilesGroup's own
            // comment), or a real .syx file is highlighted inside the
            // Import list.
            bool dexed_files_ready = load_browsing_files_ && dexed_files_group_open_
                                       && (dexed_preset_folder_open_
                                           || dexed_files_group_ == DexedFilesGroup::User);
            bool can_hold_load = browsing_load
                                  && (load_new_selected_ || dexed_files_ready
                                      || (dexed_import_browsing_ && dexed_import_file_count_ > 0));

            if(pod_->button2.Pressed() && (choosing_save || can_hold_load))
            {
                float       held     = pod_->button2.TimeHeldMs();
                int         w = (int)(Clampf(held / 800.f, 0.f, 1.f) * (disp_->Width() - 2));
                const char* hold_msg = choosing_save
                                           ? "Hold: Save..."
                                           : browsing_import ? "Hold: Import..." : "Hold: Load...";
                disp_->SetCursor(0, 20);
                WriteUpper(hold_msg);
                disp_->DrawRect(0, 30, disp_->Width() - 1, 34, true, false);
                if(w > 0)
                    disp_->DrawRect(1, 31, w, 33, true, true);
                DrawControlRow(kFooterRow1Y, false, kFooterDividerY, "", "", "", "");
                DrawControlRow(kFooterRow2Y, true, kFooterInterRowDividerY, "", "", "", "");
                break;
            }

            if(choosing_save)
            {
                bool can_overwrite
                    = dexed_loaded_preset_slot_ > DexedSynth::GetNumFactoryPresets();
                char line1[24], line2[16];
                snprintf(line1, sizeof(line1), "%c Overwrite%s", !save_as_new_ ? '>' : ' ',
                          can_overwrite ? "" : " (n/a)");
                snprintf(line2, sizeof(line2), "%c Save New", save_as_new_ ? '>' : ' ');
                disp_->SetCursor(0, 16);
                WriteUpper(line1);
                disp_->SetCursor(0, 28);
                WriteUpper(line2);
            }
            else if(load_chooser)
            {
                char line1[24], line2[24], line3[16];
                bool files_sel = !load_new_selected_ && !dexed_import_selected_;
                snprintf(line1, sizeof(line1), "%c Files", files_sel ? '>' : ' ');
                snprintf(line2, sizeof(line2), "%c Import", dexed_import_selected_ ? '>' : ' ');
                snprintf(line3, sizeof(line3), "%c Load New", load_new_selected_ ? '>' : ' ');
                disp_->SetCursor(0, 14);
                WriteUpper(line1);
                disp_->SetCursor(0, 24);
                WriteUpper(line2);
                disp_->SetCursor(0, 34);
                WriteUpper(line3);
            }
            else if(browsing_import)
            {
                char line2[24];
                if(dexed_import_file_count_ <= 0)
                    snprintf(line2, sizeof(line2), "No .syx in DXIMPORT/");
                else
                    snprintf(line2, sizeof(line2), "Import: %.14s",
                              dexed_import_names_[dexed_import_cursor_]);
                disp_->SetCursor(0, 20);
                WriteUpper(line2);
            }
            else if(browsing_group)
            {
                // Tom Thumb (see font_tomthumb.h), not the normal
                // WriteUpper()/Font_6x8 every other line on this page
                // uses -- 4 rows don't fit this body area at 8px/row,
                // Tom Thumb's own 6px row height does.
                char line1[16], line2[16], line3[16], line4[16];
                snprintf(line1, sizeof(line1), "%c Roms",
                          dexed_files_group_ == DexedFilesGroup::Roms ? '>' : ' ');
                snprintf(line2, sizeof(line2), "%c Dexed",
                          dexed_files_group_ == DexedFilesGroup::Dexed ? '>' : ' ');
                snprintf(line3, sizeof(line3), "%c Imports",
                          dexed_files_group_ == DexedFilesGroup::Imports ? '>' : ' ');
                snprintf(line4, sizeof(line4), "%c User",
                          dexed_files_group_ == DexedFilesGroup::User ? '>' : ' ');
                TomThumbDrawText(disp_, 0, 16, line1, true);
                TomThumbDrawText(disp_, 0, 24, line2, true);
                TomThumbDrawText(disp_, 0, 32, line3, true);
                TomThumbDrawText(disp_, 0, 40, line4, true);
            }
            else if(browsing_folders)
            {
                // No "Folder: " label -- Font_6x8 is fixed 6px/char, and
                // "Folder: Woodwind 3 (64)" (23 chars) overflows the
                // 128px display (21 chars max), pushing the count off
                // the right edge. The longest real name+count ("Woodwind
                // 3 (64)", 15 chars) fits comfortably without the label.
                char        line2[24];
                const char* name;
                int         count;
                if(dexed_files_group_ == DexedFilesGroup::Imports)
                {
                    if(dexed_preset_folder_cursor_ >= 0
                       && dexed_preset_folder_cursor_ < dexed_import_folder_count_)
                    {
                        const PerformanceStore::DexedImportFolder& f
                            = dexed_import_folders_[dexed_preset_folder_cursor_];
                        name  = f.name;
                        count = f.last_slot - f.first_slot + 1;
                    }
                    else
                    {
                        name  = "(none)";
                        count = 0;
                    }
                }
                else
                {
                    int cat = ResolveDexedRealCategory();
                    name    = DexedSynth::GetFactoryCategoryName(cat);
                    count   = DexedSynth::GetFactoryCategoryCount(cat);
                }
                snprintf(line2, sizeof(line2), "%s (%d)", name, count);
                disp_->SetCursor(0, 20);
                WriteUpper(line2);
            }
            else if(browsing_load) // dexed_files_ready -- see its own comment above
            {
                int  browsed_slot = ResolveDexedPresetSlot();
                char line2[24];
                if(browsed_slot < 0)
                    snprintf(line2, sizeof(line2), "(empty)");
                else if(browsed_slot <= DexedSynth::GetNumFactoryPresets())
                    snprintf(line2, sizeof(line2), "Load: %s",
                              DexedSynth::GetFactoryPresetName(browsed_slot - 1));
                else
                {
                    // A user slot's real name lives inside its own saved
                    // file (a manual save keeps whatever name was in the
                    // live patch at save time; a SysEx import keeps the
                    // bank's own original name verbatim) -- read it once
                    // per distinct slot landed on, not once per Draw()
                    // tick, see dexed_browse_name_slot_'s own comment.
                    if(browsed_slot != dexed_browse_name_slot_)
                    {
                        DexedSynth::DexedPresetData preset;
                        const char*                  name = "?";
                        if(PerformanceStore::LoadDexedPreset(browsed_slot, &preset))
                            name = DexedSynth::GetPresetDataName(preset);
                        // Some older saves genuinely predate real embedded
                        // names (before the factory bank/name system
                        // existed) and carry blank name bytes -- fall
                        // back to the slot number rather than a bare "?"
                        // so those don't look like a fresh regression.
                        if(strcmp(name, "?") == 0)
                            // No real embedded name to show (older save,
                            // blank name bytes) -- "DEXED" matches
                            // Global:SdMgmt's own short code for this
                            // category, same shared vocabulary as every
                            // other bare-number display in the project.
                            snprintf(dexed_browse_name_buf_, sizeof(dexed_browse_name_buf_),
                                      "DEXED%d", browsed_slot);
                        else
                            snprintf(dexed_browse_name_buf_, sizeof(dexed_browse_name_buf_), "%s",
                                      name);
                        dexed_browse_name_slot_ = browsed_slot;
                    }
                    snprintf(line2, sizeof(line2), "Load: %s", dexed_browse_name_buf_);
                }
                disp_->SetCursor(0, 20);
                WriteUpper(line2);
            }
            else
            {
                char line1[24];
                if(dexed_loaded_preset_slot_ <= 0)
                    snprintf(line1, sizeof(line1), "Now: (custom)");
                else if(dexed_loaded_preset_slot_ <= DexedSynth::GetNumFactoryPresets())
                    snprintf(line1, sizeof(line1), "Now: %s",
                              DexedSynth::GetFactoryPresetName(dexed_loaded_preset_slot_ - 1));
                else
                {
                    // Live in RAM already (dexed_ is currently playing
                    // it) -- no SD read needed, unlike the "Load:" case
                    // above for a highlighted-but-not-yet-applied slot.
                    const char* name = DexedSynth::GetPresetDataName(dexed_->CapturePreset());
                    if(strcmp(name, "?") == 0)
                        // See the "Load:" case's own comment -- some
                        // older saves genuinely predate real embedded
                        // names and carry blank name bytes.
                        snprintf(line1, sizeof(line1), "Now: DEXED%d", dexed_loaded_preset_slot_);
                    else
                        snprintf(line1, sizeof(line1), "Now: %s", name);
                }
                disp_->SetCursor(0, 20);
                WriteUpper(line1);
                if(dexed_preset_status_[0] != '\0')
                {
                    disp_->SetCursor(0, 32);
                    WriteUpper(dexed_preset_status_);
                }
            }

            {
                const char* b1_label;
                if(save_load_mode_ == SaveLoadMode::Idle)
                    b1_label = "Save";
                else if(load_chooser && !load_new_selected_)
                    b1_label = "Select";
                else
                    // Back everywhere else within Files' own nested
                    // browsing (group chooser, folder list, and an open
                    // folder) -- Button2 is "Open" at the group/folder
                    // levels instead (drills deeper), see
                    // OnButton1Short()/HandleButton2()'s own comments on
                    // this split.
                    b1_label = "Back";
                const char* b2_label;
                if(save_load_mode_ == SaveLoadMode::Idle)
                    b2_label = "Load";
                else if(choosing_save)
                    b2_label = "Hold=Save";
                else if(browsing_group || browsing_folders)
                    b2_label = "Open";
                else if(dexed_files_ready)
                    // "Preview/Load" -- ShortPressFn/LongPressFn, same
                    // convention as Layer:Status's "Pause/Overdub".
                    b2_label = "Preview/Load";
                else if(browsing_import && can_hold_load)
                    b2_label = "Hold=Import";
                else if(can_hold_load)
                    b2_label = "Hold=Load";
                else
                    b2_label = "";

                DrawControlRow(kFooterRow1Y, false, kFooterDividerY,
                                 (choosing_save || browsing_load) ? "Scroll" : "", "", "", "");
                DrawControlRow(kFooterRow2Y, true, kFooterInterRowDividerY, b1_label, "", "",
                                 b2_label);
            }
            break;
        }
        default: break;
    }
}

void Ui::DrawEnsembleScreen()
{
    if(!ensemble_)
        return;

    // "Ensmbl" not "Ensemble", and "Regist" not "Registration" -- same
    // ~14-char budget as every other engine's own title row (beat
    // indicator reserves the right 41px), confirmed too long on real
    // hardware with the full names.
    const char* page_name = "Regist";
    switch(ensemble_param_page_)
    {
        case EnsembleParamPage::Registration: page_name = "Regist"; break;
        case EnsembleParamPage::Env: page_name = "Env"; break;
        case EnsembleParamPage::Ensemble: page_name = "Chorus"; break;
        case EnsembleParamPage::Filter: page_name = "Filter"; break;
        case EnsembleParamPage::Mix: page_name = "Mix"; break;
        case EnsembleParamPage::FX: page_name = "FX"; break;
        case EnsembleParamPage::MIDI: page_name = "MIDI"; break;
        case EnsembleParamPage::Preset: page_name = "Preset"; break;
        default: break;
    }
    char title[24];
    snprintf(title, sizeof(title), "Ensmbl:%s", page_name);
    disp_->SetCursor(0, 0);
    WriteUpper(title);
    DrawBeatIndicator(disp_->Width() - 41, 0, 3);
    disp_->DrawLine(0, 9, disp_->Width() - 1, 9, true);

    switch(ensemble_param_page_)
    {
        case EnsembleParamPage::Registration:
        {
            // Organ-stop names, same order as EnsembleSynth::
            // SetDrawbarLevel()'s own idx (0=Saw8' .. 6=Saw1').
            static const char* const kDrawbarNames[EnsembleSynth::kNumDrawbars]
                = {"Saw 8'", "Sq 8'", "Saw 4'", "Sq 4'", "Saw 2'", "Sq 2'", "Saw 1'"};
            int idx = ensemble_drawbar_index_;
            if(idx < 0 || idx >= EnsembleSynth::kNumDrawbars)
                idx = 0;
            TomThumbDrawText(disp_, 0, 15, kDrawbarNames[idx], true);

            // 7 real vertical faders, one per drawbar -- K1 (above) picks
            // which one is selected, K2 edits that one's level; drawn
            // instead of a scope so the whole registration is visible at
            // a glance, same "see every channel, not just the one you're
            // touching" reasoning Screen::Mixer's own bars already use.
            const int kBarTop = 19, kBarBottom = kFooterDividerY - 2;
            const int kBarGap = 3;
            const int kBarW
                = (disp_->Width() - kBarGap * (EnsembleSynth::kNumDrawbars - 1))
                  / EnsembleSynth::kNumDrawbars;
            const int kRowW = kBarW * EnsembleSynth::kNumDrawbars
                               + kBarGap * (EnsembleSynth::kNumDrawbars - 1);
            const int kX0   = (disp_->Width() - kRowW) / 2;
            for(int i = 0; i < EnsembleSynth::kNumDrawbars; i++)
            {
                int x0 = kX0 + i * (kBarW + kBarGap);
                if(i == idx)
                    disp_->DrawRect(x0 - 1, kBarTop - 1, x0 + kBarW, kBarBottom + 1, true, false);
                DrawMixerVBar(x0, kBarTop, kBarW, kBarBottom - kBarTop + 1,
                               ensemble_->GetDrawbarLevel(i), false);
            }

            char val[8];
            snprintf(val, sizeof(val), "%d%%",
                      (int)(ensemble_->GetDrawbarLevel(idx) * 100.f + 0.5f));
            DrawControlRow(kFooterRow1Y, false, kFooterDividerY, "Select", "", val, "Level");
            DrawControlRow(kFooterRow2Y, true, kFooterInterRowDividerY, "", "", "", "");
            break;
        }
        case EnsembleParamPage::Env:
        {
            // Same shape as GranularParamPage::ADSR's own graphic -- all
            // four stages always shown together.
            DrawAdsrShape(12, kFooterDividerY - 2, ensemble_->GetAttackSeconds(),
                          ensemble_->GetDecaySeconds(), ensemble_->GetSustain01(),
                          ensemble_->GetReleaseSeconds());
            if(!ensemble_env_target_sr_)
            {
                char a_val[8], d_val[8];
                snprintf(a_val, sizeof(a_val), "%d%%",
                          (int)(ensemble_->GetAttack01() * 100.f + 0.5f));
                snprintf(d_val, sizeof(d_val), "%d%%",
                          (int)(ensemble_->GetDecay01() * 100.f + 0.5f));
                DrawControlRow(kFooterRow1Y, false, kFooterDividerY, "Attack", a_val, d_val,
                                 "Decay");
            }
            else
            {
                char s_val[8], r_val[8];
                snprintf(s_val, sizeof(s_val), "%d%%",
                          (int)(ensemble_->GetSustain01() * 100.f + 0.5f));
                snprintf(r_val, sizeof(r_val), "%d%%",
                          (int)(ensemble_->GetRelease01() * 100.f + 0.5f));
                DrawControlRow(kFooterRow1Y, false, kFooterDividerY, "Sustain", s_val, r_val,
                                 "Release");
            }
            DrawControlRow(kFooterRow2Y, true, kFooterInterRowDividerY,
                             ensemble_env_target_sr_ ? "AD" : "AD*", "", "",
                             ensemble_env_target_sr_ ? "SR*" : "SR");
            break;
        }
        case EnsembleParamPage::Ensemble:
        {
            DrawOscilloscope(12, kFooterDividerY - 2, ensemble_scope_buf_, ensemble_scope_capacity_);
            char depth_val[8], rate_val[8];
            snprintf(depth_val, sizeof(depth_val), "%d%%",
                      (int)(ensemble_->GetChorusDepth01() * 100.f + 0.5f));
            snprintf(rate_val, sizeof(rate_val), "%d%%",
                      (int)(ensemble_->GetChorusRate01() * 100.f + 0.5f));
            DrawControlRow(kFooterRow1Y, false, kFooterDividerY, "Depth", depth_val, rate_val,
                             "Rate");
            DrawControlRow(kFooterRow2Y, true, kFooterInterRowDividerY, "", "", "", "");
            break;
        }
        case EnsembleParamPage::Filter:
        {
            char mode_line[20];
            snprintf(mode_line, sizeof(mode_line), "Mode: %s",
                      FilterModeName(ensemble_->GetFilterMode()));
            TomThumbDrawText(disp_, 0, 15, mode_line, true);
            DrawOscilloscope(18, kFooterDividerY - 2, ensemble_scope_buf_, ensemble_scope_capacity_);
            char cutoff_val[8], res_val[8];
            snprintf(cutoff_val, sizeof(cutoff_val), "%d%%",
                      (int)(ensemble_->GetFilterCutoff01() * 100.f + 0.5f));
            snprintf(res_val, sizeof(res_val), "%d%%",
                      (int)(ensemble_->GetFilterResonance01() * 100.f + 0.5f));
            DrawControlRow(kFooterRow1Y, false, kFooterDividerY, "Cutoff", cutoff_val, res_val,
                             "Res");
            DrawControlRow(kFooterRow2Y, true, kFooterInterRowDividerY, "Cycle mode", "", "", "");
            break;
        }
        case EnsembleParamPage::Mix:
        {
            DrawOscilloscope(12, kFooterDividerY - 2, ensemble_scope_buf_, ensemble_scope_capacity_);
            char out_val[8], pan_val[8];
            snprintf(out_val, sizeof(out_val), "%d%%",
                      (int)(ensemble_->GetOutputLevel01() * 100.f + 0.5f));
            snprintf(pan_val, sizeof(pan_val), "%d%%",
                      (int)(ensemble_->GetPan01() * 100.f + 0.5f));
            DrawControlRow(kFooterRow1Y, false, kFooterDividerY, "Level", out_val, pan_val, "Pan");
            DrawControlRow(kFooterRow2Y, true, kFooterInterRowDividerY, "", "", "", "");
            break;
        }
        case EnsembleParamPage::FX:
        {
            if(!ensemble_fx_target_delay_)
            {
                char send_val[8], size_val[8];
                snprintf(send_val, sizeof(send_val), "%d%%",
                          (int)(ensemble_->GetReverbSend01() * 100.f + 0.5f));
                snprintf(size_val, sizeof(size_val), "%d%%",
                          (int)(reverb_size01_ * 100.f + 0.5f));
                TomThumbDrawText(disp_, 0, 15, "Reverb", true);
                DrawControlRow(kFooterRow1Y, false, kFooterDividerY, "Send", send_val, size_val,
                                 "Size");
            }
            else
            {
                char send_val[8], time_val[8];
                snprintf(send_val, sizeof(send_val), "%d%%",
                          (int)(ensemble_->GetDelaySend01() * 100.f + 0.5f));
                snprintf(time_val, sizeof(time_val), "%d%%",
                          (int)(delay_time01_ * 100.f + 0.5f));
                TomThumbDrawText(disp_, 0, 15, "Delay", true);
                DrawControlRow(kFooterRow1Y, false, kFooterDividerY, "Send", send_val, time_val,
                                 "Time");
            }
            DrawOscilloscope(18, kFooterDividerY - 2, ensemble_scope_buf_, ensemble_scope_capacity_);
            DrawControlRow(kFooterRow2Y, true, kFooterInterRowDividerY,
                             ensemble_fx_target_delay_ ? "Reverb" : "Reverb*", "", "",
                             ensemble_fx_target_delay_ ? "Delay*" : "Delay");
            break;
        }
        case EnsembleParamPage::MIDI:
        {
            char chan_val[8];
            if(ensemble_midi_channel_ < 0)
                snprintf(chan_val, sizeof(chan_val), "Omni");
            else
                snprintf(chan_val, sizeof(chan_val), "%d", ensemble_midi_channel_ + 1);
            char wheel_line[24];
            snprintf(wheel_line, sizeof(wheel_line), "Wheel: %s", ensemble_->GetModWheelTargetName());
            TomThumbDrawText(disp_, 0, 15, wheel_line, true);
            DrawOscilloscope(18, kFooterDividerY - 2, ensemble_scope_buf_, ensemble_scope_capacity_);
            DrawControlRow(kFooterRow1Y, false, kFooterDividerY, "Channel", chan_val, "", "");
            DrawControlRow(kFooterRow2Y, true, kFooterInterRowDividerY, "Cycle wheel tgt", "", "", "");
            break;
        }
        case EnsembleParamPage::Preset:
        {
            if(!PerformanceStore::IsCardPresent())
            {
                disp_->SetCursor(0, 20);
                WriteUpper("No card");
                DrawControlRow(kFooterRow1Y, false, kFooterDividerY, "", "", "", "");
                DrawControlRow(kFooterRow2Y, true, kFooterInterRowDividerY, "Retry", "", "", "");
                break;
            }
            if(ensemble_preset_slots_dirty_)
                RefreshEnsemblePresetSlots();

            int  factory_count = EnsembleSynth::GetNumFactoryPresets();
            bool choosing_save = save_load_mode_ == SaveLoadMode::ChoosingSave;
            bool browsing_load = save_load_mode_ == SaveLoadMode::BrowsingLoad;
            bool load_chooser  = browsing_load && !load_browsing_files_;
            bool can_hold_load = browsing_load && (load_browsing_files_ || load_new_selected_);

            // Same "Button2 hold takes over the whole display" idiom as
            // GranularParamPage::Preset -- Ensemble presets are a single
            // small struct (no audio), so this never actually runs long,
            // but the same shape keeps this in sync with every other
            // Preset page.
            if(pod_->button2.Pressed() && (choosing_save || can_hold_load))
            {
                float       held     = pod_->button2.TimeHeldMs();
                int         w        = (int)(Clampf(held / 800.f, 0.f, 1.f) * (disp_->Width() - 2));
                const char* hold_msg = choosing_save ? "Hold: Save..." : "Hold: Load...";
                disp_->SetCursor(0, 20);
                WriteUpper(hold_msg);
                disp_->DrawRect(0, 30, disp_->Width() - 1, 34, true, false);
                if(w > 0)
                    disp_->DrawRect(1, 31, w, 33, true, true);
                DrawControlRow(kFooterRow1Y, false, kFooterDividerY, "", "", "", "");
                DrawControlRow(kFooterRow2Y, true, kFooterInterRowDividerY, "", "", "", "");
                break;
            }

            if(choosing_save)
            {
                bool can_overwrite = ensemble_loaded_preset_slot_ > factory_count;
                char line1[24], line2[16];
                snprintf(line1, sizeof(line1), "%c Overwrite%s", !save_as_new_ ? '>' : ' ',
                          can_overwrite ? "" : " (n/a)");
                snprintf(line2, sizeof(line2), "%c Save New", save_as_new_ ? '>' : ' ');
                disp_->SetCursor(0, 16);
                WriteUpper(line1);
                disp_->SetCursor(0, 28);
                WriteUpper(line2);
            }
            else if(load_chooser)
            {
                // Unlike Grains' own Preset page, Files is always a real
                // option here (the 8-preset factory bank always exists).
                char line1[24], line2[16];
                snprintf(line1, sizeof(line1), "%c Files", !load_new_selected_ ? '>' : ' ');
                snprintf(line2, sizeof(line2), "%c Load New", load_new_selected_ ? '>' : ' ');
                disp_->SetCursor(0, 16);
                WriteUpper(line1);
                disp_->SetCursor(0, 28);
                WriteUpper(line2);
            }
            else if(browsing_load)
            {
                // Drilled into the combined [factory][user] list --
                // cursor < factory_count shows the factory preset's real
                // name, otherwise the real SD slot number.
                char line2[24];
                if(ensemble_preset_cursor_ < factory_count)
                    snprintf(line2, sizeof(line2), "Load: %s",
                              EnsembleSynth::GetFactoryPresetName(ensemble_preset_cursor_));
                else
                    // "ENSM" matches Global:SdMgmt's own short code for
                    // this category -- one shared vocabulary, see
                    // GRAINS/DEXED's own sites above.
                    snprintf(line2, sizeof(line2), "Load: ENSM%d",
                              ensemble_preset_user_slots_[ensemble_preset_cursor_
                                                           - factory_count]);
                disp_->SetCursor(0, 20);
                WriteUpper(line2);
            }
            else
            {
                char line1[24];
                if(ensemble_loaded_preset_slot_ <= 0)
                    snprintf(line1, sizeof(line1), "Now: (custom)");
                else if(ensemble_loaded_preset_slot_ <= factory_count)
                    snprintf(line1, sizeof(line1), "Now: %s",
                              EnsembleSynth::GetFactoryPresetName(ensemble_loaded_preset_slot_ - 1));
                else
                    snprintf(line1, sizeof(line1), "Now: ENSM%d", ensemble_loaded_preset_slot_);
                disp_->SetCursor(0, 20);
                WriteUpper(line1);
                if(ensemble_preset_status_[0] != '\0')
                {
                    disp_->SetCursor(0, 32);
                    WriteUpper(ensemble_preset_status_);
                }
            }

            {
                const char* b1_label;
                if(save_load_mode_ == SaveLoadMode::Idle)
                    b1_label = "Save";
                else if(load_chooser && !load_new_selected_)
                    b1_label = "Select";
                else
                    b1_label = "Back";
                const char* b2_label;
                if(save_load_mode_ == SaveLoadMode::Idle)
                    b2_label = "Load";
                else if(choosing_save)
                    b2_label = "Hold=Save";
                else if(can_hold_load)
                    b2_label = "Hold=Load";
                else
                    b2_label = "";

                DrawControlRow(kFooterRow1Y, false, kFooterDividerY,
                                 (choosing_save || browsing_load) ? "Scroll" : "", "", "", "");
                DrawControlRow(kFooterRow2Y, true, kFooterInterRowDividerY, b1_label, "", "",
                                 b2_label);
            }
            break;
        }
        default: break;
    }
}

void Ui::DrawPageTitle(const char* title, bool unsaved)
{
    disp_->SetCursor(0, 0);
    WriteUpper(title);
    if(unsaved)
        TomThumbDrawText(disp_, (int)strlen(title) * 6 + 1, 5, "*", true);
}

Ui::EditSnapshot Ui::SampleSynthSnapshot() const
{
    EditSnapshot s;
    if(!sample_synth_)
        return s;
    SampleSynth::SampleSynthPresetData p = sample_synth_->CapturePreset();
    s.Add(p.attack01); s.Add(p.decay01); s.Add(p.sustain01); s.Add(p.release01);
    s.Add(p.filter_mode); s.Add(p.filter_cutoff01); s.Add(p.filter_res01);
    s.Add(p.output_level01); s.Add(p.pan01); s.Add(p.reverb_send01); s.Add(p.delay_send01);
    s.Add(p.mono_choke);
    if(sample_synth_mode_ == SampleSynthMode::Voice)
    {
        s.Add(sample_synth_imp_tune_semis_);
        s.Add(sample_synth_imp_mono_);
    }
    else
    {
        s.Add(sample_synth_setup_applied_slices_);
        s.Add(sample_synth_setup_tune_semis_);
    }
    return s;
}

void Ui::MarkSampleSynthClean()
{
    if(sample_synth_mode_ == SampleSynthMode::Voice)
        sample_synth_voice_baseline_ = SampleSynthSnapshot();
    else
        sample_synth_drum_baseline_ = SampleSynthSnapshot();
}

bool Ui::SampleSynthUnsaved() const
{
    bool voice = sample_synth_mode_ == SampleSynthMode::Voice;
    if(voice && sample_synth_capture_unsaved_)
        return true;
    const EditSnapshot& base = voice ? sample_synth_voice_baseline_ : sample_synth_drum_baseline_;
    return base.len > 0 && !(SampleSynthSnapshot() == base);
}

void Ui::DrawSampleSynthScreen()
{
    if(!sample_synth_)
        return;

    const char* page_name = "Filter";
    switch(sample_synth_param_page_)
    {
        case SampleSynthParamPage::Filter: page_name = "Filter"; break;
        case SampleSynthParamPage::Env: page_name = "Env"; break;
        case SampleSynthParamPage::Mix: page_name = "Mix"; break;
        case SampleSynthParamPage::Setup: page_name = "Setup"; break;
        case SampleSynthParamPage::FX: page_name = "SR-FX"; break;
        case SampleSynthParamPage::MIDI: page_name = "MIDI"; break;
        case SampleSynthParamPage::Capture: page_name = "Capture"; break;
        case SampleSynthParamPage::Trim: page_name = "Trim"; break;
        case SampleSynthParamPage::Tune: page_name = "Tune"; break;
        case SampleSynthParamPage::Import: page_name = "Import"; break;
        case SampleSynthParamPage::Preset: page_name = "Preset"; break;
        case SampleSynthParamPage::User: page_name = "User"; break;
        default: break;
    }
    char title[24];
    snprintf(title, sizeof(title), "%s:%s",
              sample_synth_mode_ == SampleSynthMode::Voice ? "SMPVO" : "SMPDR", page_name);
    DrawPageTitle(title, SampleSynthUnsaved());
    DrawBeatIndicator(disp_->Width() - 41, 0, 3);
    disp_->DrawLine(0, 9, disp_->Width() - 1, 9, true);

    switch(sample_synth_param_page_)
    {
        case SampleSynthParamPage::Filter:
        {
            char mode_line[20];
            snprintf(mode_line, sizeof(mode_line), "Mode: %s",
                      FilterModeName(sample_synth_->GetFilterMode()));
            TomThumbDrawText(disp_, 0, 15, mode_line, true);
            DrawOscilloscope(18, kFooterDividerY - 2, sample_synth_scope_buf_,
                               sample_synth_scope_capacity_);

            char cutoff_val[8], res_val[8];
            snprintf(cutoff_val, sizeof(cutoff_val), "%d%%",
                      (int)(sample_synth_->GetFilterCutoff01() * 100.f + 0.5f));
            snprintf(res_val, sizeof(res_val), "%d%%",
                      (int)(sample_synth_->GetFilterResonance01() * 100.f + 0.5f));
            DrawControlRow(kFooterRow1Y, false, kFooterDividerY, "Cutoff", cutoff_val, res_val,
                             "Res");
            DrawControlRow(kFooterRow2Y, true, kFooterInterRowDividerY, "Cycle mode", "", "", "");
            break;
        }
        case SampleSynthParamPage::Env:
        {
            // Same merged-shape idiom as GranularParamPage::ADSR's own
            // DrawAdsrShape() call -- all four stages shown together,
            // not just whichever pair the toggle currently targets.
            const int kBandTop = 22, kBandBottom = 44;
            DrawAdsrShape(kBandTop, kBandBottom, sample_synth_->GetAttackSeconds(),
                          sample_synth_->GetDecaySeconds(), sample_synth_->GetSustain01(),
                          sample_synth_->GetReleaseSeconds());
            if(!sample_synth_adsr_target_sr_)
            {
                char a_val[8], d_val[8];
                snprintf(a_val, sizeof(a_val), "%d%%",
                          (int)(sample_synth_->GetAttack01() * 100.f + 0.5f));
                snprintf(d_val, sizeof(d_val), "%d%%",
                          (int)(sample_synth_->GetDecay01() * 100.f + 0.5f));
                DrawControlRow(kFooterRow1Y, false, kFooterDividerY, "Attack", a_val, d_val,
                                 "Decay");
            }
            else
            {
                char s_val[8], r_val[8];
                snprintf(s_val, sizeof(s_val), "%d%%",
                          (int)(sample_synth_->GetSustain01() * 100.f + 0.5f));
                snprintf(r_val, sizeof(r_val), "%d%%",
                          (int)(sample_synth_->GetRelease01() * 100.f + 0.5f));
                DrawControlRow(kFooterRow1Y, false, kFooterDividerY, "Sustain", s_val, r_val,
                                 "Release");
            }
            DrawControlRow(kFooterRow2Y, true, kFooterInterRowDividerY,
                             sample_synth_adsr_target_sr_ ? "AD" : "AD*", "", "",
                             sample_synth_adsr_target_sr_ ? "SR*" : "SR");
            break;
        }
        case SampleSynthParamPage::Mix:
        {
            char out_val[8], pan_val[8];
            snprintf(out_val, sizeof(out_val), "%d%%",
                      (int)(sample_synth_->GetOutputLevel01() * 100.f + 0.5f));
            snprintf(pan_val, sizeof(pan_val), "%d%%",
                      (int)(sample_synth_->GetPan01() * 100.f + 0.5f));
            DrawOscilloscope(18, kFooterDividerY - 2, sample_synth_scope_buf_,
                               sample_synth_scope_capacity_);
            DrawControlRow(kFooterRow1Y, false, kFooterDividerY, "Level", out_val, pan_val,
                             "Pan");
            DrawControlRow(kFooterRow2Y, true, kFooterInterRowDividerY, "", "", "", "");
            break;
        }
        case SampleSynthParamPage::Setup:
        {
            // Mono/Choke moved here from Mix -- see
            // SampleSynthPresetData::mono_choke's own comment. Same
            // status-line position every other page's own first
            // TomThumb line uses right under the header divider
            // (TomThumbDrawText's y is the text's BASELINE, not its
            // top -- row_top=10 -> baseline=10+kTomThumbRowHeight-1=15,
            // same as Filter's own mode_line above) -- naming just
            // which state is currently selected, same "the button
            // label names the option, the status line names what's
            // active" split as Env's own AD/AD*-SR/SR* footer.
            TomThumbDrawText(disp_, 0, 15, sample_synth_->GetMonoChoke() ? "Mono" : "Poly", true);
            // 4x4 grid, one cell per possible slice (0-16) -- filled
            // left-to-right/top-to-bottom up to the current *pending*
            // count (not yet-applied, see that knob's own comment), so
            // turning Knob2 fills it in live before Button2 commits.
            // Vertically centered in the body area between the status
            // line above and kFooterDividerY below.
            {
                constexpr int kCols = 4, kRows = 4, kCellW = 16, kCellH = 5, kGap = 2;
                constexpr int kGridW = kCols * kCellW + (kCols - 1) * kGap;
                constexpr int kGridH = kRows * kCellH + (kRows - 1) * kGap;
                constexpr int kBodyTop = 18; // same body-top every other page's own
                                              // oscilloscope uses below its status line
                int x0 = (disp_->Width() - kGridW) / 2;
                int y0 = kBodyTop + ((kFooterDividerY - kBodyTop) - kGridH) / 2;
                int pending = sample_synth_setup_pending_slices_;
                if(pending < 0)
                    pending = 0;
                if(pending > kCols * kRows)
                    pending = kCols * kRows;
                for(int cell = 0; cell < kCols * kRows; cell++)
                {
                    int cx = x0 + (cell % kCols) * (kCellW + kGap);
                    int cy = y0 + (cell / kCols) * (kCellH + kGap);
                    disp_->DrawRect(cx, cy, cx + kCellW - 1, cy + kCellH - 1, true, false);
                    if(cell < pending)
                        disp_->DrawRect(cx + 1, cy + 1, cx + kCellW - 2, cy + kCellH - 2, true,
                                          true);
                }
            }
            char tune_val[8], slices_val[12];
            snprintf(tune_val, sizeof(tune_val), "%+d", (int)sample_synth_setup_tune_semis_);
            // Pending vs applied -- see sample_synth_setup_pending_slices_'s
            // own comment. Shows "pending->applied" once they differ, so
            // it's clear Button2 hasn't been confirmed yet.
            if(sample_synth_setup_pending_slices_ == sample_synth_setup_applied_slices_)
                snprintf(slices_val, sizeof(slices_val), "%d",
                          sample_synth_setup_pending_slices_);
            else
                snprintf(slices_val, sizeof(slices_val), "%d->%d",
                          sample_synth_setup_applied_slices_,
                          sample_synth_setup_pending_slices_);
            DrawControlRow(kFooterRow1Y, false, kFooterDividerY, "Tune", tune_val, slices_val,
                             "Slices");
            DrawControlRow(kFooterRow2Y, true, kFooterInterRowDividerY, "Mono/Poly", "", "",
                             "Hold=Splice");
            break;
        }
        case SampleSynthParamPage::FX:
        {
            if(!sample_synth_fx_target_delay_)
            {
                char send_val[8], size_val[8];
                snprintf(send_val, sizeof(send_val), "%d%%",
                          (int)(sample_synth_->GetReverbSend01() * 100.f + 0.5f));
                snprintf(size_val, sizeof(size_val), "%d%%",
                          (int)(reverb_size01_ * 100.f + 0.5f));
                TomThumbDrawText(disp_, 0, 15, "Reverb", true);
                DrawControlRow(kFooterRow1Y, false, kFooterDividerY, "Send", send_val, size_val,
                                 "Size");
            }
            else
            {
                char send_val[8], time_val[8];
                snprintf(send_val, sizeof(send_val), "%d%%",
                          (int)(sample_synth_->GetDelaySend01() * 100.f + 0.5f));
                snprintf(time_val, sizeof(time_val), "%d%%",
                          (int)(delay_time01_ * 100.f + 0.5f));
                TomThumbDrawText(disp_, 0, 15, "Delay", true);
                DrawControlRow(kFooterRow1Y, false, kFooterDividerY, "Send", send_val, time_val,
                                 "Time");
            }
            DrawOscilloscope(18, kFooterDividerY - 2, sample_synth_scope_buf_,
                               sample_synth_scope_capacity_);
            DrawControlRow(kFooterRow2Y, true, kFooterInterRowDividerY,
                             sample_synth_fx_target_delay_ ? "Reverb" : "Reverb*", "", "",
                             sample_synth_fx_target_delay_ ? "Delay*" : "Delay");
            break;
        }
        case SampleSynthParamPage::MIDI:
        {
            char chan_val[8];
            if(sample_synth_midi_channel_ < 0)
                snprintf(chan_val, sizeof(chan_val), "Omni");
            else
                snprintf(chan_val, sizeof(chan_val), "%d", sample_synth_midi_channel_ + 1);
            // No body text of its own needed (title already says "MIDI")
            // -- same idiom as DexedParamPage::MIDI's own draw block.
            DrawOscilloscope(12, kFooterDividerY - 2, sample_synth_scope_buf_,
                               sample_synth_scope_capacity_);
            DrawControlRow(kFooterRow1Y, false, kFooterDividerY, "Channel", chan_val, "", "");
            DrawControlRow(kFooterRow2Y, true, kFooterInterRowDividerY, "", "", "", "");
            break;
        }
        case SampleSynthParamPage::Capture:
        {
            // Same shape as GranularParamPage::Capture's own draw block
            // -- hold-to-confirm progress bar for From Layer/Import,
            // Direct Record's own "Recording %" bar, otherwise the
            // source line + whatever's relevant to it.
            if(pod_->button2.Pressed()
               && (sample_synth_capture_source_ >= 0 || sample_synth_capture_source_ == -2))
            {
                float held = pod_->button2.TimeHeldMs();
                int   w    = (int)(Clampf(held / 800.f, 0.f, 1.f) * (disp_->Width() - 2));
                const char* hold_msg
                    = sample_synth_capture_source_ == -2 ? "Hold: Import..." : "Hold: Capture...";
                TomThumbDrawText(disp_, 0, 15, hold_msg, true);
                disp_->DrawRect(0, 20, disp_->Width() - 1, 24, true, false);
                if(w > 0)
                    disp_->DrawRect(1, 21, w, 23, true, true);
                DrawControlRow(kFooterRow1Y, false, kFooterDividerY, "", "", "", "");
                DrawControlRow(kFooterRow2Y, true, kFooterInterRowDividerY, "", "", "", "");
                break;
            }

            char src_line[24];
            if(sample_synth_capture_source_ >= 0)
                snprintf(src_line, sizeof(src_line), "Source: Layer %d",
                          sample_synth_capture_source_ + 1);
            else if(sample_synth_capture_source_ == -2)
                snprintf(src_line, sizeof(src_line), "Source: Import");
            else
                snprintf(src_line, sizeof(src_line), "Source: Direct Record");
            TomThumbDrawText(disp_, 0, 15, src_line, true);

            if(sample_synth_capture_source_ == -2 && sample_synth_import_files_dirty_)
                RefreshSampleSynthImportFiles();

            bool recording = sample_synth_capture_source_ == -1 && sample_synth_capturing_
                              && *sample_synth_capturing_;
            if(sample_synth_capture_source_ == -2)
            {
                if(sample_synth_capture_status_[0] != '\0')
                {
                    TomThumbDrawText(disp_, 0, 22, sample_synth_capture_status_, true);
                }
                else if(sample_synth_import_file_count_ > 0)
                {
                    constexpr int kVisibleRows = 4;
                    int           count        = sample_synth_import_file_count_;
                    int           start        = sample_synth_import_cursor_ - kVisibleRows / 2;
                    if(start > count - kVisibleRows)
                        start = count - kVisibleRows;
                    if(start < 0)
                        start = 0;
                    int rows = count < kVisibleRows ? count : kVisibleRows;
                    for(int row = 0; row < rows; row++)
                    {
                        int  idx      = start + row;
                        bool selected = idx == sample_synth_import_cursor_;
                        char line[64];
                        snprintf(line, sizeof(line), "%c%s", selected ? '>' : ' ',
                                  sample_synth_import_names_[idx]);
                        while(line[0] != '\0' && TomThumbAdvanceWidth(line) > disp_->Width())
                            line[strlen(line) - 1] = '\0';
                        TomThumbDrawText(disp_, 0, 22 + row * 6, line, true);
                    }
                }
                else
                {
                    TomThumbDrawText(disp_, 0, 22, "No .wav files in IMPORT/", true);
                }
            }
            else if(recording)
            {
                float frac = sample_synth_capture_capacity_ > 0
                                 ? (float)*sample_synth_capture_write_pos_
                                       / (float)sample_synth_capture_capacity_
                                 : 0.f;
                char rec_line[20];
                snprintf(rec_line, sizeof(rec_line), "Recording %d%%",
                          (int)(frac * 100.f + 0.5f));
                TomThumbDrawText(disp_, 0, 22, rec_line, true);

                const int kBarX0 = 0, kBarX1 = disp_->Width() - 1, kBarY0 = 28, kBarY1 = 36;
                disp_->DrawRect(kBarX0, kBarY0, kBarX1, kBarY1, true, false);
                int fill_w
                    = (int)(Clampf(frac, 0.f, 1.f) * (float)(kBarX1 - kBarX0 - 1) + 0.5f);
                if(fill_w > 0)
                    disp_->DrawRect(kBarX0 + 1, kBarY0 + 1, kBarX0 + fill_w, kBarY1 - 1, true,
                                      true);
            }
            else
            {
                if(sample_synth_capture_status_[0] != '\0')
                    TomThumbDrawText(disp_, 0, 22, sample_synth_capture_status_, true);
                DrawWaveform(sample_synth_capture_peaks_, false, 0.f);
            }

            const char* hold_label = "Hold=Record";
            if(sample_synth_capture_source_ >= 0)
                hold_label = "Hold=Capture";
            else if(sample_synth_capture_source_ == -2)
                hold_label = "Hold=Import";
            DrawControlRow(kFooterRow1Y, false, kFooterDividerY, "", "", "", "");
            DrawControlRow(kFooterRow2Y, true, kFooterInterRowDividerY, "Cycle Source", "", "",
                             hold_label);
            break;
        }
        case SampleSynthParamPage::Trim:
        {
            if(pod_->button2.Pressed())
            {
                float held = pod_->button2.TimeHeldMs();
                int   w    = (int)(Clampf(held / 800.f, 0.f, 1.f) * (disp_->Width() - 2));
                TomThumbDrawText(disp_, 0, 15, "Hold: Save...", true);
                disp_->DrawRect(0, 20, disp_->Width() - 1, 24, true, false);
                if(w > 0)
                    disp_->DrawRect(1, 21, w, 23, true, true);
                DrawControlRow(kFooterRow1Y, false, kFooterDividerY, "", "", "", "");
                DrawControlRow(kFooterRow2Y, true, kFooterInterRowDividerY, "", "", "", "");
                break;
            }

            char line[24];
            snprintf(line, sizeof(line), "Start%d%% End%d%%",
                      (int)(sample_synth_trim_start01_ * 100.f + 0.5f),
                      (int)(sample_synth_trim_end01_ * 100.f + 0.5f));
            TomThumbDrawText(disp_, 0, 15, line, true);

            // Reuses DrawGranularWaveform()'s own two-marker drawing as-
            // is -- a generic Ui:: method despite the name, not Grains-
            // coupled (see sample_synth_capture_peaks_'s own comment).
            DrawGranularWaveform(sample_synth_capture_peaks_, sample_synth_trim_start01_,
                                  sample_synth_trim_end01_, sample_synth_capture_full_len_ > 0);

            if(sample_synth_capture_status_[0] != '\0')
                TomThumbDrawText(disp_, 0, 22, sample_synth_capture_status_, true);

            char s_val[8], e_val[8];
            snprintf(s_val, sizeof(s_val), "%d%%", (int)(sample_synth_trim_start01_ * 100.f + 0.5f));
            snprintf(e_val, sizeof(e_val), "%d%%", (int)(sample_synth_trim_end01_ * 100.f + 0.5f));
            DrawControlRow(kFooterRow1Y, false, kFooterDividerY, "Start", s_val, e_val, "End");
            DrawControlRow(kFooterRow2Y, true, kFooterInterRowDividerY, "Play", "", "",
                             "Hold=Save");
            break;
        }
        case SampleSynthParamPage::Tune:
        {
            // Semitones, not the raw root note -- same display convention
            // as GranularParamPage::Tune (0 = "play back exactly as
            // captured", +-24 = +-2 octaves either side). K1/B1 = the
            // captured wav, K2/B2 = whatever's loaded via Import/
            // Preset/User -- see sample_synth_imp_tune_semis_'s own
            // comment for why these are two independent, remembered
            // values rather than one shared pair.
            int  cap_semis = 60 - (int)sample_synth_tune_root_note_;
            char mono_line[28];
            snprintf(mono_line, sizeof(mono_line), "Cap:%s  Imp:%s",
                      sample_synth_cap_mono_ ? "Mono" : "Poly",
                      sample_synth_imp_mono_ ? "Mono" : "Poly");
            TomThumbDrawText(disp_, 0, 15, mono_line, true);
            char cap_val[8], imp_val[8];
            snprintf(cap_val, sizeof(cap_val), "%+d", cap_semis);
            snprintf(imp_val, sizeof(imp_val), "%+d", (int)sample_synth_imp_tune_semis_);
            DrawControlRow(kFooterRow1Y, false, kFooterDividerY, "Cap Tune", cap_val, imp_val,
                             "Imp Tune");
            DrawControlRow(kFooterRow2Y, true, kFooterInterRowDividerY, "Mono/Poly-Cap", "", "",
                             "Mono/Poly-Imp");
            break;
        }
        case SampleSynthParamPage::Import:
        {
            // Wholly separate from Capture's own Import source -- see
            // SampleSynthParamPage::Import's own doc comment in ui.h.
            if(pod_->button2.Pressed())
            {
                float held = pod_->button2.TimeHeldMs();
                int   w    = (int)(Clampf(held / 800.f, 0.f, 1.f) * (disp_->Width() - 2));
                TomThumbDrawText(disp_, 0, 15, "Hold: Import...", true);
                disp_->DrawRect(0, 20, disp_->Width() - 1, 24, true, false);
                if(w > 0)
                    disp_->DrawRect(1, 21, w, 23, true, true);
                DrawControlRow(kFooterRow1Y, false, kFooterDividerY, "", "", "", "");
                DrawControlRow(kFooterRow2Y, true, kFooterInterRowDividerY, "", "", "", "");
                break;
            }

            const char* target_name = "All";
            if(sample_synth_import_target_ == SampleSynthImportTarget::Kit)
                target_name = "Drums"; // DRUM PACK + BREAK PACK IMPORT
            else if(sample_synth_import_target_ == SampleSynthImportTarget::Sound)
                target_name = "Sounds";
            char target_line[24];
            snprintf(target_line, sizeof(target_line), "Target: %s", target_name);
            TomThumbDrawText(disp_, 0, 15, target_line, true);

            disp_->SetCursor(0, 24);
            WriteUpper("From * PACK IMPORT");

            if(sample_synth_pack_import_status_[0] != '\0')
            {
                disp_->SetCursor(0, 36);
                WriteUpper(sample_synth_pack_import_status_);
            }

            DrawControlRow(kFooterRow1Y, false, kFooterDividerY, "", "", "", "");
            DrawControlRow(kFooterRow2Y, true, kFooterInterRowDividerY, "Cycle target", "", "",
                             "Hold=Import");
            break;
        }
        case SampleSynthParamPage::Preset:
        {
            // LOAD-ONLY (see this page's own doc comment) -- no Save/
            // ChoosingSave/Files-vs-New branches, just Idle vs
            // BrowsingLoad, same trimmed-down shape as this page's
            // simplicity calls for.
            if(!PerformanceStore::IsCardPresent())
            {
                disp_->SetCursor(0, 20);
                WriteUpper("No card");
                DrawControlRow(kFooterRow1Y, false, kFooterDividerY, "", "", "", "");
                DrawControlRow(kFooterRow2Y, true, kFooterInterRowDividerY, "Retry", "", "", "");
                break;
            }
            // Two levels in both faces: banks, then the open bank's
            // items -- see sample_synth_sound_category_'s own comment in ui.h.
            bool drum_face          = !sample_synth_preset_mode_sounds_;
            bool at_category_select = sample_synth_sound_category_ < 0;

            if(sample_synth_banks_dirty_ || sample_synth_banks_drum_ != drum_face)
            {
                sample_synth_bank_count_
                    = ListFaceSampleBanks(drum_face, sample_synth_banks_, kMaxListedSampleBanks);
                sample_synth_banks_drum_  = drum_face;
                sample_synth_banks_dirty_ = false;
                if(sample_synth_sound_category_cursor_ >= sample_synth_bank_count_)
                    sample_synth_sound_category_cursor_ = 0;
                if(sample_synth_sound_category_ >= sample_synth_bank_count_)
                    sample_synth_sound_category_ = -1; // that bank's gone -- back to the list
                at_category_select = sample_synth_sound_category_ < 0;
            }
            if(!at_category_select && sample_synth_sounds_dirty_)
            {
                const SampleBankEntry& e = sample_synth_banks_[sample_synth_sound_category_];
                sample_synth_sound_count_ = PerformanceStore::ListSampleBankItems(
                    e.type, e.number, sample_synth_sound_names_, kMaxSampleSynthSoundsListed);
                if(sample_synth_kit_cursor_ >= sample_synth_sound_count_)
                    sample_synth_kit_cursor_ = 0;
                sample_synth_sounds_dirty_ = false;
            }

            int  total = at_category_select ? 0 : sample_synth_sound_count_;
            bool browsing_load = save_load_mode_ == SaveLoadMode::BrowsingLoad;
            bool in_folder     = browsing_load && !at_category_select;

            // Which face's content, and once drilled in, which bank.
            char mode_line[24];
            if(!at_category_select)
                SampleBankEntryLabel(sample_synth_banks_[sample_synth_sound_category_], mode_line,
                                     sizeof(mode_line));
            else
                snprintf(mode_line, sizeof(mode_line), "%s",
                          drum_face ? "Kits+Breaks" : "Sounds+Captured");
            TomThumbDrawText(disp_, 0, 15, mode_line, true);

            if(browsing_load)
            {
                char line2[24];
                if(at_category_select && sample_synth_bank_count_ > 0)
                    SampleBankEntryLabel(sample_synth_banks_[sample_synth_sound_category_cursor_],
                                         line2, sizeof(line2));
                else if(!at_category_select && total > 0)
                {
                    // "> " marks the sound that's loaded right now -- you stay
                    // in the folder after loading, so it's worth showing.
                    const char* nm  = sample_synth_sound_names_[sample_synth_kit_cursor_];
                    const char* ref = sample_synth_loaded_kit_ref_;
                    const char* sl  = strrchr(ref, '/');
                    bool loaded = drum_face ? (sl && strcmp(sl + 1, nm) == 0)
                                            : strcmp(sample_synth_loaded_sound_name_, nm) == 0;
                    snprintf(line2, sizeof(line2), "%s%s", loaded ? "> " : "", nm);
                }
                else
                    snprintf(line2, sizeof(line2), "(none on card)");
                disp_->SetCursor(0, 20);
                WriteUpper(line2);
            }
            else
            {
                char line1[24];
                const char* loaded_name = sample_synth_preset_mode_sounds_
                                              ? sample_synth_loaded_sound_name_
                                              : sample_synth_loaded_kit_name_;
                if(loaded_name[0] == '\0')
                    snprintf(line1, sizeof(line1), "Now: (none)");
                else
                    snprintf(line1, sizeof(line1), "Now: %s", loaded_name);
                disp_->SetCursor(0, 20);
                WriteUpper(line1);
                if(sample_synth_kit_status_[0] != '\0')
                {
                    disp_->SetCursor(0, 32);
                    WriteUpper(sample_synth_kit_status_);
                }
            }

            // Shared browse convention: K1 scrolls folders, K2 scrolls the
            // sounds inside one, B1 backs out a level, B2 opens/loads.
            DrawControlRow(kFooterRow1Y, false, kFooterDividerY,
                             browsing_load && at_category_select ? "Scroll" : "", "", "",
                             in_folder ? "Scroll" : "");
            DrawControlRow(kFooterRow2Y, true, kFooterInterRowDividerY,
                             save_load_mode_ == SaveLoadMode::Idle ? "" : "Back", "", "",
                             save_load_mode_ == SaveLoadMode::Idle
                                 ? "Load"
                                 : (at_category_select
                                        ? (sample_synth_bank_count_ > 0 ? "Open" : "")
                                        : (total > 0 ? "Load" : "")));
            break;
        }
        case SampleSynthParamPage::User:
        {
            if(!PerformanceStore::IsCardPresent())
            {
                disp_->SetCursor(0, 20);
                WriteUpper("No card");
                DrawControlRow(kFooterRow1Y, false, kFooterDividerY, "", "", "", "");
                DrawControlRow(kFooterRow2Y, true, kFooterInterRowDividerY, "Retry", "", "", "");
                break;
            }
            // Whichever face's own User-preset list -- see
            // SampleSynthVoiceUserPreset/SampleSynthDrumUserPreset's
            // own comments.
            bool is_voice = sample_synth_mode_ == SampleSynthMode::Voice;
            if(is_voice && sample_synth_voice_user_dirty_)
            {
                sample_synth_voice_user_count_ = PerformanceStore::ListSampleSynthVoiceUserPresets(
                    sample_synth_voice_user_numbers_,
                    PerformanceStore::kMaxSampleSynthVoiceUserPresets);
                if(sample_synth_voice_user_cursor_ >= sample_synth_voice_user_count_)
                    sample_synth_voice_user_cursor_
                        = sample_synth_voice_user_count_ > 0 ? sample_synth_voice_user_count_ - 1
                                                                 : 0;
                sample_synth_voice_user_dirty_ = false;
            }
            else if(!is_voice && sample_synth_drum_user_dirty_)
            {
                sample_synth_drum_user_count_ = PerformanceStore::ListSampleSynthDrumUserPresets(
                    sample_synth_drum_user_numbers_,
                    PerformanceStore::kMaxSampleSynthDrumUserPresets);
                if(sample_synth_drum_user_cursor_ >= sample_synth_drum_user_count_)
                    sample_synth_drum_user_cursor_
                        = sample_synth_drum_user_count_ > 0 ? sample_synth_drum_user_count_ - 1 : 0;
                sample_synth_drum_user_dirty_ = false;
            }

            int         count  = is_voice ? sample_synth_voice_user_count_
                                             : sample_synth_drum_user_count_;
            int         cursor = is_voice ? sample_synth_voice_user_cursor_
                                             : sample_synth_drum_user_cursor_;
            int*        numbers
                = is_voice ? sample_synth_voice_user_numbers_ : sample_synth_drum_user_numbers_;
            int         loaded_slot = is_voice ? sample_synth_voice_user_loaded_slot_
                                                   : sample_synth_drum_user_loaded_slot_;
            const char* status      = is_voice ? sample_synth_voice_user_status_
                                                   : sample_synth_drum_user_status_;

            bool choosing_save = save_load_mode_ == SaveLoadMode::ChoosingSave;
            bool browsing_load = save_load_mode_ == SaveLoadMode::BrowsingLoad;

            // Saving keeps its hold-to-confirm bar; loading is a plain tap.
            if(pod_->button2.Pressed() && choosing_save)
            {
                float       held     = pod_->button2.TimeHeldMs();
                int         w        = (int)(Clampf(held / 800.f, 0.f, 1.f) * (disp_->Width() - 2));
                const char* hold_msg = "Hold: Save...";
                disp_->SetCursor(0, 20);
                WriteUpper(hold_msg);
                disp_->DrawRect(0, 30, disp_->Width() - 1, 34, true, false);
                if(w > 0)
                    disp_->DrawRect(1, 31, w, 33, true, true);
                DrawControlRow(kFooterRow1Y, false, kFooterDividerY, "", "", "", "");
                DrawControlRow(kFooterRow2Y, true, kFooterInterRowDividerY, "", "", "", "");
                break;
            }

            TomThumbDrawText(disp_, 0, 15, is_voice ? "Sound Presets" : "Kit Presets", true);

            if(choosing_save)
            {
                bool can_overwrite = loaded_slot > 0;
                char line1[24], line2[16];
                snprintf(line1, sizeof(line1), "%c Overwrite%s", !save_as_new_ ? '>' : ' ',
                          can_overwrite ? "" : " (n/a)");
                snprintf(line2, sizeof(line2), "%c Save New", save_as_new_ ? '>' : ' ');
                disp_->SetCursor(0, 24);
                WriteUpper(line1);
                disp_->SetCursor(0, 34);
                WriteUpper(line2);
            }
            else if(browsing_load)
            {
                // "SMPVO"/"SMPDR" match Global:SdMgmt's own short codes
                // for these two categories -- one shared vocabulary, see
                // GRAINS/DEXED/ENSM's own sites above.
                char line2[24];
                if(count > 0)
                    snprintf(line2, sizeof(line2), "%s%s%d", numbers[cursor] == loaded_slot ? "> " : "",
                              is_voice ? "SMPVO" : "SMPDR", numbers[cursor]);
                else
                    snprintf(line2, sizeof(line2), "(none saved yet)");
                disp_->SetCursor(0, 24);
                WriteUpper(line2);
            }
            else
            {
                char line1[24];
                if(loaded_slot <= 0)
                    snprintf(line1, sizeof(line1), "Now: (none)");
                else
                    snprintf(line1, sizeof(line1), "Now: %s%d", is_voice ? "SMPVO" : "SMPDR",
                              loaded_slot);
                disp_->SetCursor(0, 24);
                WriteUpper(line1);
                if(status[0] != '\0')
                {
                    disp_->SetCursor(0, 36);
                    WriteUpper(status);
                }
            }

            DrawControlRow(kFooterRow1Y, false, kFooterDividerY, "", "", "",
                             browsing_load ? "Scroll" : "");
            DrawControlRow(kFooterRow2Y, true, kFooterInterRowDividerY,
                             save_load_mode_ == SaveLoadMode::Idle ? "Save" : "Back", "", "",
                             save_load_mode_ == SaveLoadMode::Idle
                                 ? "Load"
                                 : (browsing_load ? (count > 0 ? "Load" : "")
                                                  : (choosing_save ? "Hold=Save" : "")));
            break;
        }
        default: break;
    }
}

void Ui::DrawDexedOperatorScreen()
{
    if(!dexed_)
        return;

    // Real HW op number (6-index), same convention the Algo diagram
    // already uses -- confirmed from the real DX7 SysEx spec that array
    // index 0 is HW OP6, index 5 is OP1.
    int hw_op = 6 - dexed_op_index_;
    int base  = dexed_op_index_ * 21;

    const char* page_name = "Ratio";
    switch(dexed_op_page_)
    {
        case DexedOpParamPage::Algo: page_name = "Algo"; break;
        case DexedOpParamPage::RatioLevel: page_name = "Ratio"; break;
        case DexedOpParamPage::Detune: page_name = "Detune"; break;
        case DexedOpParamPage::EgRate: page_name = "EG Rate"; break;
        case DexedOpParamPage::EgLevel: page_name = "EG Level"; break;
        case DexedOpParamPage::AmpModSens: page_name = "AmpSens"; break;
        case DexedOpParamPage::VelocitySens: page_name = "VelSens"; break;
        case DexedOpParamPage::KeybRateScale: page_name = "RateScale"; break;
        default: break;
    }
    char title[24];
    snprintf(title, sizeof(title), "Op %d:%s", hw_op, page_name);
    disp_->SetCursor(0, 0);
    WriteUpper(title);
    DrawBeatIndicator(disp_->Width() - 41, 0, 3);
    disp_->DrawLine(0, 9, disp_->Width() - 1, 9, true);

    switch(dexed_op_page_)
    {
        case DexedOpParamPage::Algo:
        {
            // Read-only here -- see DexedOpParamPage::Algo's own comment
            // for why Button1/Button2 don't cycle the algorithm on this
            // page the way Screen::Dexed's own Algo page lets them.
            DrawAlgorithmDiagram(dexed_->GetPatchByte(134));
            DrawControlRow(kFooterRow1Y, false, kFooterDividerY, "", "", "", "");
            DrawControlRow(kFooterRow2Y, true, kFooterInterRowDividerY, "Op", "", "", "");
            break;
        }
        case DexedOpParamPage::RatioLevel:
        {
            char ratio_val[8], level_val[8];
            snprintf(ratio_val, sizeof(ratio_val), "%d", dexed_->GetPatchByte(base + 18));
            snprintf(level_val, sizeof(level_val), "%d", dexed_->GetPatchByte(base + 16));
            DrawControlRow(kFooterRow1Y, false, kFooterDividerY, "Ratio", ratio_val, level_val,
                             "Level");
            DrawControlRow(kFooterRow2Y, true, kFooterInterRowDividerY, "Op", "", "", "");
            break;
        }
        case DexedOpParamPage::Detune:
        {
            char val[8];
            snprintf(val, sizeof(val), "%d", dexed_->GetPatchByte(base + 20));
            disp_->SetCursor(0, 20);
            WriteUpper("7 = centered");
            DrawControlRow(kFooterRow1Y, false, kFooterDividerY, "Detune", val, "", "");
            DrawControlRow(kFooterRow2Y, true, kFooterInterRowDividerY, "Op", "", "", "");
            break;
        }
        case DexedOpParamPage::EgRate:
        {
            // Real DX7 R1-R4 aren't literally an ADSR envelope, but
            // naming them Attack/Decay/Sustain/Release (matching this
            // same AD/SR-toggle idiom's own "AD"/"SR" pairing) reads far
            // more clearly than "Rate1..4" -- requested directly after
            // real hardware testing.
            bool    sr = dexed_op_egrate_target_sr_;
            uint8_t rates[4], levels[4];
            for(int i = 0; i < 4; i++)
            {
                rates[i]  = dexed_->GetPatchByte(base + i);
                levels[i] = dexed_->GetPatchByte(base + 4 + i);
            }
            DrawDx7EnvelopeShape(14, 44, rates, levels);
            char v1[8], v2[8];
            snprintf(v1, sizeof(v1), "%d", dexed_->GetPatchByte(sr ? base + 2 : base + 0));
            snprintf(v2, sizeof(v2), "%d", dexed_->GetPatchByte(sr ? base + 3 : base + 1));
            DrawControlRow(kFooterRow1Y, false, kFooterDividerY, sr ? "Sustain" : "Attack", v1, v2,
                             sr ? "Release" : "Decay");
            DrawControlRow(kFooterRow2Y, true, kFooterInterRowDividerY, "Op", "", "", "AD/SR");
            break;
        }
        case DexedOpParamPage::EgLevel:
        {
            // Same Attack/Decay/Sustain/Release naming as the EgRate
            // page above, and for the same reason -- the title bar
            // ("Op N:EG Level" vs "...EG Rate") already disambiguates
            // level from rate, so reusing identical stage names here
            // instead of "Level1..4" is consistent, not ambiguous.
            bool    sr = dexed_op_eglevel_target_sr_;
            uint8_t rates[4], levels[4];
            for(int i = 0; i < 4; i++)
            {
                rates[i]  = dexed_->GetPatchByte(base + i);
                levels[i] = dexed_->GetPatchByte(base + 4 + i);
            }
            DrawDx7EnvelopeShape(14, 44, rates, levels);
            char v1[8], v2[8];
            snprintf(v1, sizeof(v1), "%d", dexed_->GetPatchByte(sr ? base + 6 : base + 4));
            snprintf(v2, sizeof(v2), "%d", dexed_->GetPatchByte(sr ? base + 7 : base + 5));
            DrawControlRow(kFooterRow1Y, false, kFooterDividerY, sr ? "Sustain" : "Attack", v1, v2,
                             sr ? "Release" : "Decay");
            DrawControlRow(kFooterRow2Y, true, kFooterInterRowDividerY, "Op", "", "", "AD/SR");
            break;
        }
        case DexedOpParamPage::AmpModSens:
        {
            char val[8];
            snprintf(val, sizeof(val), "%d", dexed_->GetPatchByte(base + 14) & 3);
            disp_->SetCursor(0, 20);
            WriteUpper("0 = no wheel effect"); // 19 chars -- Font_6x8 only
                                                 // fits 21 at this width,
                                                 // "...has no effect" (23)
                                                 // ran 10px off the right
                                                 // edge
            DrawControlRow(kFooterRow1Y, false, kFooterDividerY, "AmpSens", val, "", "");
            DrawControlRow(kFooterRow2Y, true, kFooterInterRowDividerY, "Op", "", "", "");
            break;
        }
        case DexedOpParamPage::VelocitySens:
        {
            char val[8];
            snprintf(val, sizeof(val), "%d", dexed_->GetPatchByte(base + 15));
            disp_->SetCursor(0, 20);
            WriteUpper("0 = no vel. response");
            DrawControlRow(kFooterRow1Y, false, kFooterDividerY, "VelSens", val, "", "");
            DrawControlRow(kFooterRow2Y, true, kFooterInterRowDividerY, "Op", "", "", "");
            break;
        }
        case DexedOpParamPage::KeybRateScale:
        {
            char val[8];
            snprintf(val, sizeof(val), "%d", dexed_->GetPatchByte(base + 13));
            disp_->SetCursor(0, 20);
            WriteUpper("0 = flat across keys"); // 22 chars ("...all
                                                  // keys") ran 4px off
                                                  // the right edge
            DrawControlRow(kFooterRow1Y, false, kFooterDividerY, "RateScale", val, "", "");
            DrawControlRow(kFooterRow2Y, true, kFooterInterRowDividerY, "Op", "", "", "");
            break;
        }
        default: break;
    }
}

void Ui::DrawMixerScreen()
{
    DrawBeatIndicator(disp_->Width() - 41, 0, 3);
    disp_->DrawLine(0, 9, disp_->Width() - 1, 9, true);

    if(mixer_position_ == kNumMixerChannels) // Scope stop
    {
        disp_->SetCursor(0, 0);
        WriteUpper("Mixer:Scope");
        TomThumbDrawText(disp_, 0, 15, "Final Mix", true);
        DrawOscilloscope(19, 44, master_scope_buf_, master_scope_capacity_);
        DrawControlRow(kFooterRow1Y, false, kFooterDividerY, "", "", "", "");
        DrawControlRow(kFooterRow2Y, true, kFooterInterRowDividerY, "", "", "", "");
        return;
    }

    // A real channel -- whichever group of 2 it belongs to (groups of 2,
    // not 4, so each channel gets a real 3rd bar for Peak alongside its
    // existing Vol/Rev pair -- see DrawMixerVBar()'s own doc comment for
    // why Peak lives on this page instead of a separate one).
    bool is_master = mixer_position_ == kNumMixerChannels - 1;
    char title[24];
    snprintf(title, sizeof(title), "Mixer:%s", MixerChannelName(mixer_position_));
    disp_->SetCursor(0, 0);
    WriteUpper(title);

    int       group_start = (mixer_position_ / 2) * 2;
    const int kColW       = disp_->Width() / 2;
    // Centered between the header divider (y=9) and footer divider
    // (kFooterDividerY=46) -- no per-bar "Vol"/"Rev"/"Pk" text any more
    // (the footer already spells those out for the selected channel),
    // so the freed-up room goes to real vertical centering instead of
    // cramming everything against the top.
    const int kBarW = 12, kBarGap = 2, kBarY0 = 20, kBarH = 18;
    const int kPanY = 39, kPanH = 4;

    for(int col = 0; col < 2; col++)
    {
        int ch = group_start + col;
        // 9 real channels (odd) since Ensemble was added -- the last
        // group has only one real channel (Master, alone); leave the
        // second column blank rather than reading/drawing channel 9
        // (out of range, MixerChannelName()/MixerGet*01() would all
        // silently fall through to their own "?"/0.f defaults, a real
        // but confusing-looking bug rather than a crash).
        if(ch >= kNumMixerChannels)
            break;
        bool selected = ch == mixer_position_;
        int  col_x    = kColW * col;
        int  bars_w   = kBarW * 3 + kBarGap * 2;
        int  bars_x0  = col_x + (kColW - bars_w) / 2;

        if(selected)
            disp_->DrawRect(col_x + 1, 11, col_x + kColW - 2, 44, true, false);

        const char* name   = MixerChannelName(ch);
        int         nw     = TomThumbAdvanceWidth(name);
        int         name_x = col_x + (kColW - nw) / 2;
        TomThumbDrawText(disp_, name_x, 17, name, true);

        bool  channel_is_master = ch == kNumMixerChannels - 1;
        float vol01             = MixerGetVolume01(ch);
        float second01          = channel_is_master ? reverb_size01_ : MixerGetSend01(ch);

        DrawMixerVBar(bars_x0, kBarY0, kBarW, kBarH, vol01);
        DrawMixerVBar(bars_x0 + kBarW + kBarGap, kBarY0, kBarW, kBarH, second01);
        // Peak -- live signal level, not a fader setting (see
        // MixerGetLevel01()'s own comment); the clip-indicator latch
        // lives on this bar specifically, not Vol/Rev, so it's always
        // unambiguous which bar is "the meter."
        DrawMixerVBar(bars_x0 + (kBarW + kBarGap) * 2, kBarY0, kBarW, kBarH,
                       MixerGetLevel01(ch), MixerIsClipping(ch));

        if(MixerChannelHasPan(ch))
        {
            float pan01 = MixerGetPan01(ch);
            disp_->DrawRect(bars_x0, kPanY, bars_x0 + bars_w - 1, kPanY + kPanH - 1, true, false);
            int tick_x = bars_x0 + 1
                         + (int)(Clampf(pan01, 0.f, 1.f) * (float)(bars_w - 3) + 0.5f);
            disp_->DrawRect(tick_x, kPanY + 1, tick_x, kPanY + kPanH - 2, true, true);
        }
    }

    char vol_val[8], second_val[8];
    snprintf(vol_val, sizeof(vol_val), "%d%%",
              (int)(MixerGetVolume01(mixer_position_) * 100.f + 0.5f));
    if(is_master)
    {
        snprintf(second_val, sizeof(second_val), "%d%%", (int)(reverb_size01_ * 100.f + 0.5f));
        DrawControlRow(kFooterRow1Y, false, kFooterDividerY, "Vol", vol_val, second_val, "RevSz");
        DrawControlRow(kFooterRow2Y, true, kFooterInterRowDividerY, "", "", "", "");
    }
    else if(!mixer_target_reverb_)
    {
        if(MixerChannelHasPan(mixer_position_))
        {
            char pan_val[8];
            snprintf(pan_val, sizeof(pan_val), "%d%%",
                      (int)(MixerGetPan01(mixer_position_) * 100.f + 0.5f));
            DrawControlRow(kFooterRow1Y, false, kFooterDividerY, "Vol", vol_val, pan_val, "Pan");
            DrawControlRow(kFooterRow2Y, true, kFooterInterRowDividerY, "Vol/Pan*", "", "", "Rev");
        }
        else
        {
            // No channel currently lacks Pan besides Master (handled
            // above via is_master) -- kept as a real fallback rather
            // than an assert, in case that ever changes again.
            DrawControlRow(kFooterRow1Y, false, kFooterDividerY, "Vol", vol_val, "", "");
            DrawControlRow(kFooterRow2Y, true, kFooterInterRowDividerY, "Vol*", "", "", "Rev");
        }
    }
    else
    {
        snprintf(second_val, sizeof(second_val), "%d%%",
                  (int)(MixerGetSend01(mixer_position_) * 100.f + 0.5f));
        DrawControlRow(kFooterRow1Y, false, kFooterDividerY, "Rev", second_val, "", "");
        DrawControlRow(kFooterRow2Y, true, kFooterInterRowDividerY, "Vol/Pan", "", "", "Rev*");
    }
}

void Ui::DrawAdsrShape(int top, int bottom, float attack_s, float decay_s, float sustain01,
                        float release_s)
{
    // Duplicated from GranularEngine's own private range (same "duplicate
    // rather than expose internals" convention this project already uses
    // elsewhere) -- must match GranularEngine::kMinAdsrSeconds/
    // kMaxAdsrSeconds exactly or this graph and the footer's own seconds
    // readout would silently disagree about where a given knob position
    // lands.
    const float kMinAdsrSeconds = 0.005f;
    const float kMaxAdsrSeconds = 3.f;

    sustain01    = Clampf(sustain01, 0.f, 1.f);
    float attack01
        = Clampf((attack_s - kMinAdsrSeconds) / (kMaxAdsrSeconds - kMinAdsrSeconds), 0.f, 1.f);
    float decay01
        = Clampf((decay_s - kMinAdsrSeconds) / (kMaxAdsrSeconds - kMinAdsrSeconds), 0.f, 1.f);
    float release01
        = Clampf((release_s - kMinAdsrSeconds) / (kMaxAdsrSeconds - kMinAdsrSeconds), 0.f, 1.f);

    const int total_w = disp_->Width() - 2; // matches every other band's 1..Width()-2 span

    // Each stage gets a fixed, EQUAL quarter of the width -- a genuine
    // zone boundary that never moves regardless of what any of the other
    // knobs are set to (turning Decay never visibly shifts the Attack
    // ramp or Release ramp, even though neither changed).
    int x0    = 1;
    int zone1 = x0 + total_w * 1 / 4; // end of Attack's zone
    int zone2 = x0 + total_w * 2 / 4; // end of Decay's zone
    int zone3 = x0 + total_w * 3 / 4; // end of Sustain's zone
    int zone4 = x0 + total_w;         // end of Release's zone

    const int bottom_y  = bottom;
    const int top_y     = top;
    const int sustain_y = bottom - (int)(sustain01 * (float)(bottom - top));

    int x_attack_peak  = x0 + (int)(attack01 * (float)(zone1 - x0));
    int x_decay_peak   = zone1 + (int)(decay01 * (float)(zone2 - zone1));
    int x_release_peak = zone3 + (int)(release01 * (float)(zone4 - zone3));

    disp_->DrawLine(x0, bottom_y, x_attack_peak, top_y, true);         // Attack ramp
    disp_->DrawLine(x_attack_peak, top_y, zone1, top_y, true);         // hold at peak
    disp_->DrawLine(zone1, top_y, x_decay_peak, sustain_y, true);      // Decay ramp
    disp_->DrawLine(x_decay_peak, sustain_y, zone2, sustain_y, true);  // hold at sustain
    disp_->DrawLine(zone2, sustain_y, zone3, sustain_y, true);         // Sustain (flat, untimed)
    disp_->DrawLine(zone3, sustain_y, x_release_peak, bottom_y, true); // Release ramp
    disp_->DrawLine(x_release_peak, bottom_y, zone4, bottom_y, true);  // hold at 0
}

void Ui::DrawDx7EnvelopeShape(int top, int bottom, const uint8_t rates[4], const uint8_t levels[4])
{
    const int total_w = disp_->Width() - 2;
    const int x0       = 1;

    int prev_x = x0;
    int prev_y = bottom; // the envelope always starts silent before Rate1/Level1 fires

    for(int seg = 0; seg < 4; seg++)
    {
        int zone_start = x0 + total_w * seg / 4;
        int zone_end   = x0 + total_w * (seg + 1) / 4;

        // Higher rate = faster = reaches its own target level sooner
        // within the zone (a narrower ramp, more flat hold afterward) --
        // same "ramp then hold flat for the rest of the zone" idiom
        // DrawAdsrShape() above uses, just driven by a 0-99 rate byte
        // (bigger = faster) instead of a seconds value.
        float rate01 = Clampf((float)rates[seg] / 99.f, 0.f, 1.f);
        int   ramp_w = (int)((1.f - rate01) * (float)(zone_end - zone_start));
        if(ramp_w < 1)
            ramp_w = 1;
        int target_x = zone_start + ramp_w;
        if(target_x > zone_end)
            target_x = zone_end;
        int target_y
            = bottom - (int)(Clampf((float)levels[seg] / 99.f, 0.f, 1.f) * (float)(bottom - top));

        disp_->DrawLine(prev_x, prev_y, target_x, target_y, true); // this segment's own ramp
        if(target_x < zone_end)
            disp_->DrawLine(target_x, target_y, zone_end, target_y, true); // hold for the rest

        prev_x = zone_end;
        prev_y = target_y;
    }
}

void Ui::DrawAlgorithmDiagram(int algo0)
{
    // Algorithm number (top-left, below the header) plus a real box
    // diagram of its exact operator routing filling the rest of the
    // page -- generically derived per-algorithm from
    // FmCore::get_operator_routing() (never hand-authored per
    // algorithm), so this is always exactly correct for whichever of
    // the 32 is loaded.
    char line1[16];
    snprintf(line1, sizeof(line1), "ALGO %d/32", algo0 + 1);
    TomThumbDrawText(disp_, 0, 15, line1, true);

    // --- Reconstruct the signal-flow graph -------------------
    // Array index 0 = hardware OP6, index 5 = OP1 (confirmed from
    // the real DX7 SysEx spec), matching render()'s own
    // processing order (index 0..5) -- a bus can only ever be
    // read by an operator whose index is higher than whichever
    // operator(s) wrote it, so a single forward pass is enough to
    // resolve every edge and depth.
    int  input_bus[6], output_bus[6];
    bool sums[6], is_carrier[6] = {}, has_fb[6];
    for(int i = 0; i < 6; i++)
    {
        FmOperatorRouting r = FmCore::get_operator_routing((uint8_t)algo0, i);
        input_bus[i]  = r.input_bus;
        output_bus[i] = r.output_bus;
        sums[i]       = r.sums;
        has_fb[i]     = r.has_feedback;
    }

    int  edge_count[6] = {};
    int  edge_from[6][6];
    int  cur_writer_count[3] = {0, 0, 0}; // indexed by bus 1/2 (0 unused)
    int  cur_writers[3][6];
    for(int i = 0; i < 6; i++)
    {
        int ib = input_bus[i];
        if(ib != 0)
        {
            for(int k = 0; k < cur_writer_count[ib]; k++)
                edge_from[i][edge_count[i]++] = cur_writers[ib][k];
        }
        int ob = output_bus[i];
        if(ob == 0)
        {
            is_carrier[i] = true;
        }
        else if(sums[i] && cur_writer_count[ob] > 0)
        {
            cur_writers[ob][cur_writer_count[ob]++] = i;
        }
        else
        {
            cur_writers[ob][0] = i;
            cur_writer_count[ob] = 1;
        }
    }

    // Row = depth from this operator's own chain root.
    int depth[6];
    for(int i = 0; i < 6; i++)
    {
        if(edge_count[i] == 0)
        {
            depth[i] = 0;
        }
        else
        {
            int maxd = 0;
            for(int k = 0; k < edge_count[i]; k++)
            {
                int pd = depth[edge_from[i][k]];
                if(pd > maxd)
                    maxd = pd;
            }
            depth[i] = maxd + 1;
        }
    }

    // Column: each independent chain root gets the next free
    // column (in processing order, i.e. HW op6 first); an
    // operator with a single parent extends that parent's column
    // (straight chain) UNLESS the parent already has an earlier
    // child claiming that column -- a parent feeding more than
    // one child (e.g. algorithm 19's op6 feeding both op5 and
    // op4) branches its later children into fresh columns
    // instead of stacking them on top of each other. An operator
    // with more than one parent (a converging algorithm, e.g.
    // 16-18) sits centered between its parents' columns instead.
    float column[6];
    bool  column_claimed[6] = {};
    int   next_root_col     = 0;
    for(int i = 0; i < 6; i++)
    {
        if(edge_count[i] == 0)
        {
            column[i] = (float)(next_root_col++);
        }
        else if(edge_count[i] == 1)
        {
            int p = edge_from[i][0];
            if(!column_claimed[p])
            {
                column[i]         = column[p];
                column_claimed[p] = true;
            }
            else
            {
                column[i] = (float)(next_root_col++);
            }
        }
        else
        {
            float sum = 0.f;
            for(int k = 0; k < edge_count[i]; k++)
                sum += column[edge_from[i][k]];
            column[i] = sum / (float)edge_count[i];
        }
    }
    int num_columns = next_root_col;
    int max_depth   = 0;
    for(int i = 0; i < 6; i++)
        if(depth[i] > max_depth)
            max_depth = depth[i];
    int num_rows = max_depth + 1;

    // --- Pixel layout, orthogonal connectors only -------------
    // Diagram width stops short of the full screen width, and the
    // OUT bus line only spans that same reduced width -- reserves
    // a strip on the right no box or connector ever enters, so
    // the "OUT" label can sit there with no risk of collision
    // regardless of algorithm/column count.
    const int kOutLabelW = 16;
    const int kDiagTop   = 17;
    const int kOutLineY  = kFooterDividerY - 1; // 45, just above the footer rule
    const int kDiagWidth = disp_->Width() - kOutLabelW;
    const float col_pitch = (float)kDiagWidth / (float)num_columns;
    const float row_pitch = (float)(kOutLineY - kDiagTop) / (float)num_rows;
    // Width is fixed and snug around the number's own ink (3px
    // wide, see font_tomthumb.cpp's glyph table) plus a real 1px
    // gap and a 1px border on each side -- not stretched to the
    // column pitch, which has far more room to spare than the
    // number ever needs and was what made boxes read as too
    // wide. Height instead scales with row_pitch (rows are the
    // genuinely scarce dimension), so the 29 algorithms with more
    // vertical room to spare get visibly taller boxes; the floor
    // (7 = 5px ink + 1px border each side, no extra gap left) is
    // only actually reached by the three 4-row algorithms
    // (1, 2, 18), where there's no more height to give.
    const int box_w = 7;
    int       box_h = (int)row_pitch;
    if(box_h > 11) box_h = 11;
    if(box_h < 7) box_h = 7;

    int cx[6], cy[6];
    for(int i = 0; i < 6; i++)
    {
        cx[i] = (int)(col_pitch * (column[i] + 0.5f));
        cy[i] = kDiagTop + (int)(row_pitch * ((float)depth[i] + 0.5f));
    }

    // Connectors first (drawn under the boxes). A same-column
    // parent drops a plain straight vertical line into the
    // child's top edge. A cross-column parent (a converging
    // algorithm, e.g. 16-18, or a parent branching into more than
    // one child, e.g. 19) instead drops down to the CHILD's own
    // row height and runs a horizontal segment straight into the
    // middle of the child's near side edge -- no separate gutter
    // row needed above the child, which is what leaves the extra
    // vertical room for bigger boxes. Horizontal/vertical
    // segments only, as required.
    for(int i = 0; i < 6; i++)
    {
        for(int k = 0; k < edge_count[i]; k++)
        {
            int p  = edge_from[i][k];
            int py = cy[p] + box_h / 2;
            if(cx[p] == cx[i])
            {
                disp_->DrawLine(cx[p], py, cx[i], cy[i] - box_h / 2, true);
            }
            else
            {
                int side_x = cx[p] < cx[i] ? cx[i] - box_w / 2 : cx[i] + box_w / 2;
                disp_->DrawLine(cx[p], py, cx[p], cy[i], true);
                disp_->DrawLine(cx[p], cy[i], side_x, cy[i], true);
            }
        }
    }

    // OUT bus: every carrier drops a straight vertical line down
    // to one shared horizontal line at the bottom of the diagram
    // -- stops at kDiagWidth, clear of the reserved "OUT" label
    // strip (see kOutLabelW above).
    disp_->DrawLine(0, kOutLineY, kDiagWidth - 1, kOutLineY, true);
    TomThumbDrawText(disp_, kDiagWidth + 2, kOutLineY - 1, "OUT", true);
    for(int i = 0; i < 6; i++)
        if(is_carrier[i])
            disp_->DrawLine(cx[i], cy[i] + box_h / 2, cx[i], kOutLineY, true);

    // Boxes: filled = carrier (reaches the ear directly), outline
    // = modulator. The real HW operator number (6-index) is
    // punched through as "off" pixels when the box is filled so
    // it still reads clearly against the solid fill.
    for(int i = 0; i < 6; i++)
    {
        int x0 = cx[i] - box_w / 2, x1 = cx[i] + box_w / 2;
        int y0 = cy[i] - box_h / 2, y1 = cy[i] + box_h / 2;
        disp_->DrawRect(x0, y0, x1, y1, true, is_carrier[i]);
        char num[2]  = {(char)('0' + (6 - i)), 0};
        int  ink_w   = TomThumbInkWidth(num);
        // Centered on ink width (not advance width) so the
        // number sits with a true, symmetric 1px-or-more gap
        // from the border on both sides -- box_w (7) is sized
        // for the common 3px-wide digits, so '1' (2px ink) gets
        // an extra spare pixel on the right rather than being
        // pushed off-center.
        int pen_x = x0 + 1 + (box_w - 2 - ink_w) / 2;
        // Same margin-based centering vertically: ink is a fixed
        // 5px tall, baseline sits 5 rows below its top -- when
        // box_h is at its 7px floor this comes out flush against
        // the border (no room left for a gap that small), but it
        // grows into a real, symmetric gap on every algorithm
        // with a taller box to spare.
        int pen_y = y0 + 6 + (box_h - 7) / 2;
        TomThumbDrawText(disp_, pen_x, pen_y, num, !is_carrier[i]);

        // Feedback: an explicit "FB" tag beside the one operator
        // (if any) with real self-feedback -- a small pixel
        // bracket read poorly at this box size across several
        // algorithms, so this uses actual text instead, which
        // stays legible regardless of where that operator lands.
        // Feedback operators are always chain roots (no incoming
        // edges, confirmed from the routing data), so connectors
        // only ever leave from their BOTTOM -- both side edges
        // are always free for this label. Prefers the right side;
        // falls back to the left if that would run past the
        // reserved OUT-label strip.
        if(has_fb[i])
        {
            const char* fb = "FB";
            int         fb_w = TomThumbInkWidth(fb);
            int         fb_x = x1 + 2;
            if(fb_x + fb_w > kDiagWidth)
                fb_x = x0 - 2 - fb_w;
            TomThumbDrawText(disp_, fb_x, pen_y, fb, true);
        }
    }
}

void Ui::DrawOscilloscope(int top, int bottom, const float* buf, size_t capacity)
{
    const int center_y = (top + bottom) / 2;
    const int half_h    = (bottom - top) / 2;

    if(!buf || capacity == 0)
    {
        disp_->DrawLine(1, center_y, 1 + (LooperLayer::kWaveformCols - 1) * 2, center_y, true);
        return;
    }

    const size_t stride = capacity / (size_t)LooperLayer::kWaveformCols;

    // Auto-scale to the loudest sample currently in the capture -- a
    // fixed scale would either clip the trace off-screen or leave it
    // looking flat depending on how loud the pad's output currently is.
    float max_abs = 0.f;
    for(size_t i = 0; i < capacity; i++)
    {
        float a = fabsf(buf[i]);
        if(a > max_abs)
            max_abs = a;
    }
    float scale = max_abs > 0.001f ? 1.f / max_abs : 0.f;

    int prev_x = 1, prev_y = center_y;
    for(int col = 0; col < LooperLayer::kWaveformCols; col++)
    {
        size_t idx = (size_t)col * stride;
        if(idx >= capacity)
            idx = capacity - 1;
        float sample = Clampf(buf[idx] * scale, -1.f, 1.f);
        int   x      = 1 + col * 2;
        int   y      = center_y - (int)(sample * half_h);
        if(col > 0)
            disp_->DrawLine(prev_x, prev_y, x, y, true);
        prev_x = x;
        prev_y = y;
    }
}

void Ui::RefreshFileSlots()
{
    file_slot_count_ = PerformanceStore::ListSlots(file_slots_, kMaxFileSlots);
    if(file_cursor_ >= file_slot_count_)
        file_cursor_ = file_slot_count_ > 0 ? file_slot_count_ - 1 : 0;
    file_slots_dirty_ = false;
}

void Ui::TriggerSave(bool force_new)
{
    int slot = (!force_new && loaded_slot_ >= 0) ? loaded_slot_ : PerformanceStore::NextFreeSlot();
    if(slot < 0)
    {
        snprintf(file_status_, sizeof(file_status_), "Card full/missing");
        return;
    }

    // Suspend the audio engine for the whole write, same as TriggerLoad()
    // already does and for a closely related reason: Save() is a long
    // blocking main-loop call (streaming every layer's recorded audio to
    // SD), and unlike Load() this was never a correctness issue for the
    // audio itself (Save() only reads layer state, never mutates it) --
    // but a real, reproduced hardware bug traced an intermittent
    // "Fail:close9" (FatFs FR_INVALID_OBJECT, i.e. the file handle
    // itself turning up invalid right at the final f_close(), after
    // every actual write already succeeded) to the full-cost audio ISR
    // continuing to run, completely unsuspended, the entire time --
    // contending with the SD card's own SDMMC DMA transfer for the same
    // bus/timing for however long the write takes. Intermittent even
    // without any of this session's other changes (confirmed by
    // reverting everything else and still seeing it fail sometimes),
    // consistently reproducible with enough extra audio-ISR load on top.
    g_audio_suspended    = true;
    g_progress_disp      = disp_;
    file_op_in_progress_ = true;
    bool ok = PerformanceStore::Save(slot, *tempo_, layers_, num_layers_, master_volume01_,
                                       bypass_, master_filter_mode_, master_filter_cutoff01_,
                                       master_filter_res01_, reverb_size01_,
                                       bypass_reverb_send01_, reverb_predelay01_,
                                       reverb_prefilter01_, reverb_input_diffusion1_01_,
                                       reverb_input_diffusion2_01_, reverb_decay_diffusion01_,
                                       reverb_damping01_, dexed_midi_channel_,
                                       granular_midi_channel_, ensemble_midi_channel_,
                                       sample_synth_midi_channel_,
                                       &Ui::OnSaveLoadProgress);
    file_op_in_progress_ = false;
    g_progress_disp       = nullptr;
    g_audio_suspended     = false;

    if(ok)
    {
        loaded_slot_ = slot;
        snprintf(file_status_, sizeof(file_status_), "Saved %d", slot);
        file_slots_dirty_ = true; // a new slot may now exist
    }
    else
    {
        snprintf(file_status_, sizeof(file_status_), "Fail:%s", PerformanceStore::GetLastError());
    }
}

void Ui::TriggerNew()
{
    for(int i = 0; i < num_layers_; i++)
        layers_[i].Clear();
    loaded_slot_ = -1; // next Save lands in a new slot, not over the old one
    snprintf(file_status_, sizeof(file_status_), "New perf");
}

void Ui::TriggerLoad()
{
    // !load_browsing_files_ means the top-level chooser confirmed with
    // "New" highlighted (can_confirm_load only allows a confirm here when
    // that's the case -- see HandleButton2()) -- confirming it is exactly
    // TriggerNew()'s own wipe.
    if(!load_browsing_files_)
    {
        TriggerNew();
        return;
    }
    int slot = file_slots_[file_cursor_];

    // Freeze the audio engine for the whole load: it restores layers one
    // at a time and each can take a while (streaming its recorded audio
    // off the SD card), and the audio ISR keeps running throughout a
    // blocking main-loop call like this one -- without suspending it, a
    // layer already restored earlier in Load() starts playing (and the
    // tempo clock keeps ticking) while later layers are still being
    // read, so every layer would resume at a different sample offset.
    // See audio_engine.h and TempoClock::ResetPhase().
    g_audio_suspended    = true;
    g_progress_disp      = disp_;
    file_op_in_progress_ = true;
    bool ok = PerformanceStore::Load(slot, *tempo_, layers_, num_layers_, &master_volume01_,
                                       &bypass_, &master_filter_mode_, &master_filter_cutoff01_,
                                       &master_filter_res01_, &reverb_size01_,
                                       &bypass_reverb_send01_, &reverb_predelay01_,
                                       &reverb_prefilter01_, &reverb_input_diffusion1_01_,
                                       &reverb_input_diffusion2_01_,
                                       &reverb_decay_diffusion01_, &reverb_damping01_,
                                       &dexed_midi_channel_, &granular_midi_channel_,
                                       &ensemble_midi_channel_, &sample_synth_midi_channel_,
                                       &Ui::OnSaveLoadProgress);
    file_op_in_progress_ = false;
    g_progress_disp       = nullptr;
    g_audio_suspended     = false; // every layer + tempo phase is consistent now

    // Project vari-speed is a live-performance control, not part of a
    // saved performance (same rule as master volume) -- always back to
    // 1.0x after a Load, same as at boot, regardless of success/failure.
    project_speed01_      = 0.5f;
    project_speed_        = 1.f;
    speed_transport_mode_ = SpeedTransportMode::Normal;
    for(int i = 0; i < num_layers_; i++)
        layers_[i].SetFreezeActive(false);

    if(ok)
    {
        loaded_slot_ = slot;
        // PerformanceStore::Load() only writes the raw 0..1 value; Ui
        // owns the volume curve, same as ApplyKnobs()'s Home-screen case.
        master_volume_ = powf(Clampf(master_volume01_, 0.f, 1.f), 2.5f) * 1.43f;
        if(master_volume_ < 0.f)
            master_volume_ = 0.f;
        snprintf(file_status_, sizeof(file_status_), "Loaded PERF%d", slot);
    }
    else
    {
        snprintf(file_status_, sizeof(file_status_), "Fail:%s", PerformanceStore::GetLastError());
    }
}

const int* Ui::SdMgmtSlots(int* out_count)
{
    switch(sd_mgmt_folder_)
    {
        case SdMgmtFolder::Performances:
            if(file_slots_dirty_)
                RefreshFileSlots();
            *out_count = file_slot_count_;
            return file_slots_;
        case SdMgmtFolder::GranularPresets:
            if(granular_preset_slots_dirty_)
                RefreshGranularPresetSlots();
            *out_count = granular_preset_user_slot_count_;
            return granular_preset_user_slots_;
        case SdMgmtFolder::DexedPresets:
            if(dexed_preset_slots_dirty_)
                RefreshDexedPresetSlots();
            *out_count = dexed_preset_user_slot_count_;
            return dexed_preset_user_slots_;
        case SdMgmtFolder::EnsemblePresets:
            if(ensemble_preset_slots_dirty_)
                RefreshEnsemblePresetSlots();
            *out_count = ensemble_preset_user_slot_count_;
            return ensemble_preset_user_slots_;
        case SdMgmtFolder::SampleSynthVoiceUserPresets:
            if(sample_synth_voice_user_dirty_)
                RefreshSampleSynthVoiceUserSlots();
            *out_count = sample_synth_voice_user_count_;
            return sample_synth_voice_user_numbers_;
        case SdMgmtFolder::SampleSynthDrumUserPresets:
            if(sample_synth_drum_user_dirty_)
                RefreshSampleSynthDrumUserSlots();
            *out_count = sample_synth_drum_user_count_;
            return sample_synth_drum_user_numbers_;
        default:
            *out_count = 0;
            return nullptr;
    }
}

void Ui::RefreshSdMgmtBanks()
{
    using PerformanceStore::SampleBank;
    if(sd_mgmt_folder_ == SdMgmtFolder::SampleSynthKits)
        sd_mgmt_bank_count_ = ListFaceSampleBanks(true, sd_mgmt_banks_, kMaxListedSampleBanks);
    else
    {
        uint8_t numbers[PerformanceStore::kMaxSampleBanks];
        int n = PerformanceStore::ListSampleBanks(SampleBank::Captured, numbers,
                                                  PerformanceStore::kMaxSampleBanks);
        for(int i = 0; i < n; i++)
        {
            sd_mgmt_banks_[i].type   = SampleBank::Captured;
            sd_mgmt_banks_[i].number = numbers[i];
        }
        sd_mgmt_bank_count_ = n;
    }
    sd_mgmt_banks_dirty_ = false;
    if(sd_mgmt_bank_ >= sd_mgmt_bank_count_)
        sd_mgmt_bank_ = -1; // that bank's gone -- back to the list
}

void Ui::RefreshSdMgmtItems()
{
    sd_mgmt_item_count_ = 0;
    if(sd_mgmt_bank_ >= 0 && sd_mgmt_bank_ < sd_mgmt_bank_count_)
    {
        const SampleBankEntry& e = sd_mgmt_banks_[sd_mgmt_bank_];
        sd_mgmt_item_count_      = PerformanceStore::ListSampleBankItems(
            e.type, e.number, sd_mgmt_item_names_, kMaxSampleSynthSoundsListed);
    }
    sd_mgmt_items_dirty_ = false;
}

int Ui::SdMgmtBankLevelCount()
{
    if(sd_mgmt_banks_dirty_)
        RefreshSdMgmtBanks();
    if(sd_mgmt_bank_ < 0)
        return sd_mgmt_bank_count_;
    if(sd_mgmt_items_dirty_)
        RefreshSdMgmtItems();
    return sd_mgmt_item_count_;
}

void Ui::RefreshSampleSynthVoiceUserSlots()
{
    sample_synth_voice_user_count_ = PerformanceStore::ListSampleSynthVoiceUserPresets(
        sample_synth_voice_user_numbers_, PerformanceStore::kMaxSampleSynthVoiceUserPresets);
    if(sample_synth_voice_user_cursor_ >= sample_synth_voice_user_count_)
        sample_synth_voice_user_cursor_
            = sample_synth_voice_user_count_ > 0 ? sample_synth_voice_user_count_ - 1 : 0;
    sample_synth_voice_user_dirty_ = false;
    if(sd_mgmt_cursor_ >= sample_synth_voice_user_count_)
        sd_mgmt_cursor_
            = sample_synth_voice_user_count_ > 0 ? sample_synth_voice_user_count_ - 1 : 0;
}

void Ui::RefreshSampleSynthDrumUserSlots()
{
    sample_synth_drum_user_count_ = PerformanceStore::ListSampleSynthDrumUserPresets(
        sample_synth_drum_user_numbers_, PerformanceStore::kMaxSampleSynthDrumUserPresets);
    if(sample_synth_drum_user_cursor_ >= sample_synth_drum_user_count_)
        sample_synth_drum_user_cursor_
            = sample_synth_drum_user_count_ > 0 ? sample_synth_drum_user_count_ - 1 : 0;
    sample_synth_drum_user_dirty_ = false;
    if(sd_mgmt_cursor_ >= sample_synth_drum_user_count_)
        sd_mgmt_cursor_
            = sample_synth_drum_user_count_ > 0 ? sample_synth_drum_user_count_ - 1 : 0;
}

namespace
{
// "CAP007.wav" -> 7; 0 for anything that isn't a CAPnnn.wav capture.
int CaptureNumberFromName(const char* name)
{
    if(strncmp(name, "CAP", 3) != 0)
        return 0;
    int n = atoi(name + 3);
    return n >= 1 && n <= PerformanceStore::kSampleBankCapacity ? n : 0;
}
} // namespace

void Ui::TriggerSdMgmtBankDuplicate()
{
    int count = SdMgmtBankLevelCount();
    if(sd_mgmt_bank_ < 0 || sd_mgmt_cursor_ >= count)
        return;
    char folder[16];
    SampleBankEntryFolder(sd_mgmt_banks_[sd_mgmt_bank_], folder, sizeof(folder));
    const char* item = sd_mgmt_item_names_[sd_mgmt_cursor_];
    char        result[PerformanceStore::kMaxSampleSynthKitRefLen] = {};
    bool        ok = false;
    g_progress_disp = disp_;
    if(sd_mgmt_folder_ == SdMgmtFolder::SampleSynthCaptures)
    {
        // Stays in the same CAPTUREDn -- if that one's full, it just
        // fails (see DuplicateSampleSynthCapture()'s own doc comment).
        int n = CaptureNumberFromName(item), new_n = -1;
        ok    = n > 0
             && PerformanceStore::DuplicateSampleSynthCapture(folder, n, &new_n,
                                                              &Ui::OnSaveLoadProgress);
        if(ok)
            snprintf(result, sizeof(result), "CAP%03d", new_n);
    }
    else
    {
        char ref[PerformanceStore::kMaxSampleSynthKitRefLen];
        snprintf(ref, sizeof(ref), "%s/%s", folder, item);
        char new_ref[PerformanceStore::kMaxSampleSynthKitRefLen];
        ok = PerformanceStore::DuplicateSampleSynthKit(ref, new_ref, sizeof(new_ref),
                                                       &Ui::OnSaveLoadProgress);
        if(ok)
            snprintf(result, sizeof(result), "%s", new_ref);
    }
    g_progress_disp = nullptr;
    if(ok)
    {
        snprintf(sd_mgmt_status_, sizeof(sd_mgmt_status_), "Copied:%s", result);
        MarkSampleSynthListsDirty();
    }
    else
        snprintf(sd_mgmt_status_, sizeof(sd_mgmt_status_), "Fail:%s",
                  PerformanceStore::GetLastError());
}

void Ui::TriggerSdMgmtBankDelete()
{
    int count = SdMgmtBankLevelCount();
    if(sd_mgmt_bank_ < 0 || sd_mgmt_cursor_ >= count)
        return;
    char folder[16];
    SampleBankEntryFolder(sd_mgmt_banks_[sd_mgmt_bank_], folder, sizeof(folder));
    char item[PerformanceStore::kMaxSampleSynthWavNameLen + 1];
    snprintf(item, sizeof(item), "%s", sd_mgmt_item_names_[sd_mgmt_cursor_]);
    bool ok = false;
    if(sd_mgmt_folder_ == SdMgmtFolder::SampleSynthCaptures)
    {
        // Only the simple case -- deleting the exact file that's
        // currently loaded clears the loaded-sound tracking. A later
        // capture shifting down a number (gap-closing compaction) leaves
        // the file itself untouched; only the "currently loaded" label
        // could show a stale filename until the next load.
        bool was_loaded = strcmp(sample_synth_loaded_sound_category_, folder) == 0
                          && strcmp(sample_synth_loaded_sound_name_, item) == 0;
        int n = CaptureNumberFromName(item);
        ok    = n > 0 && PerformanceStore::DeleteSampleSynthCapture(folder, n);
        if(ok && was_loaded)
            sample_synth_loaded_sound_name_[0] = '\0';
    }
    else
    {
        char ref[PerformanceStore::kMaxSampleSynthKitRefLen];
        snprintf(ref, sizeof(ref), "%s/%s", folder, item);
        ok = PerformanceStore::DeleteSampleSynthKit(ref);
        if(ok && strcmp(sample_synth_loaded_kit_ref_, ref) == 0)
            sample_synth_loaded_kit_ref_[0] = '\0';
        if(ok && strcmp(sample_synth_drum_kit_ref_, ref) == 0)
            sample_synth_drum_kit_ref_[0] = '\0'; // nothing left to restore on a mode switch
    }
    if(ok)
    {
        snprintf(sd_mgmt_status_, sizeof(sd_mgmt_status_), "Deleted:%s", item);
        MarkSampleSynthListsDirty();
        if(sd_mgmt_cursor_ > 0)
            sd_mgmt_cursor_--;
    }
    else
        snprintf(sd_mgmt_status_, sizeof(sd_mgmt_status_), "Fail:%s",
                  PerformanceStore::GetLastError());
}

void Ui::TriggerSdMgmtDuplicate()
{
    if(SdMgmtIsBankFolder())
    {
        TriggerSdMgmtBankDuplicate();
        return;
    }
    int        count = 0;
    const int* slots = SdMgmtSlots(&count);
    if(!slots || sd_mgmt_cursor_ >= count)
        return;
    int  slot     = slots[sd_mgmt_cursor_];
    int  new_slot = -1;
    bool ok;
    g_progress_disp = disp_;
    switch(sd_mgmt_folder_)
    {
        case SdMgmtFolder::Performances:
            ok = PerformanceStore::DuplicateSlot(slot, &new_slot, &Ui::OnSaveLoadProgress);
            break;
        case SdMgmtFolder::GranularPresets:
            ok = PerformanceStore::DuplicateGranularPreset(slot, &new_slot,
                                                              &Ui::OnSaveLoadProgress);
            break;
        case SdMgmtFolder::DexedPresets:
            ok = PerformanceStore::DuplicateDexedPreset(slot, &new_slot, &Ui::OnSaveLoadProgress);
            break;
        case SdMgmtFolder::EnsemblePresets:
            ok = PerformanceStore::DuplicateEnsemblePreset(slot, &new_slot,
                                                              &Ui::OnSaveLoadProgress);
            break;
        case SdMgmtFolder::SampleSynthVoiceUserPresets:
            ok = PerformanceStore::DuplicateSampleSynthVoiceUserPreset(slot, &new_slot,
                                                                          &Ui::OnSaveLoadProgress);
            break;
        case SdMgmtFolder::SampleSynthDrumUserPresets:
            ok = PerformanceStore::DuplicateSampleSynthDrumUserPreset(slot, &new_slot,
                                                                         &Ui::OnSaveLoadProgress);
            break;
        default: ok = false; break;
    }
    g_progress_disp = nullptr;

    if(ok)
    {
        // Dynamic, not a fixed string -- this one status line is shared
        // across all 8 SdMgmtFolder categories, so it uses whichever
        // short code the currently-selected box itself shows
        // (SdMgmtFolderShortLabel()), same vocabulary as every other
        // per-engine site.
        snprintf(sd_mgmt_status_, sizeof(sd_mgmt_status_), "Copied to %s%d",
                  SdMgmtFolderShortLabel(sd_mgmt_folder_), new_slot);
        switch(sd_mgmt_folder_)
        {
            case SdMgmtFolder::Performances: file_slots_dirty_ = true; break;
            case SdMgmtFolder::GranularPresets: granular_preset_slots_dirty_ = true; break;
            case SdMgmtFolder::DexedPresets: dexed_preset_slots_dirty_ = true; break;
            case SdMgmtFolder::EnsemblePresets: ensemble_preset_slots_dirty_ = true; break;
            case SdMgmtFolder::SampleSynthVoiceUserPresets:
                sample_synth_voice_user_dirty_ = true;
                break;
            case SdMgmtFolder::SampleSynthDrumUserPresets:
                sample_synth_drum_user_dirty_ = true;
                break;
            default: break;
        }
    }
    else
    {
        snprintf(sd_mgmt_status_, sizeof(sd_mgmt_status_), "Fail:%s",
                  PerformanceStore::GetLastError());
    }
}

void Ui::TriggerSdMgmtDelete()
{
    if(SdMgmtIsBankFolder())
    {
        TriggerSdMgmtBankDelete();
        return;
    }
    int        count = 0;
    const int* slots = SdMgmtSlots(&count);
    if(!slots || sd_mgmt_cursor_ >= count)
        return;
    int  slot = slots[sd_mgmt_cursor_];
    bool ok;
    // Passing the relevant page's own "currently loaded" tracker lets
    // Delete*() keep it correct across the gap-closing renumbering it
    // does (see its own doc comment) -- otherwise a delete elsewhere in
    // the list could silently leave e.g. the File page's "Now: N" naming
    // the wrong performance (or Overwrite writing to it) after everything
    // above it shifts down.
    switch(sd_mgmt_folder_)
    {
        case SdMgmtFolder::Performances:
            ok = PerformanceStore::DeleteSlot(slot, &loaded_slot_);
            break;
        case SdMgmtFolder::GranularPresets:
            ok = PerformanceStore::DeleteGranularPreset(slot, &granular_loaded_preset_slot_);
            break;
        case SdMgmtFolder::DexedPresets:
            ok = PerformanceStore::DeleteDexedPreset(slot, &dexed_loaded_preset_slot_);
            break;
        case SdMgmtFolder::EnsemblePresets:
            ok = PerformanceStore::DeleteEnsemblePreset(slot, &ensemble_loaded_preset_slot_);
            break;
        case SdMgmtFolder::SampleSynthVoiceUserPresets:
            ok = PerformanceStore::DeleteSampleSynthVoiceUserPreset(
                slot, &sample_synth_voice_user_loaded_slot_);
            break;
        case SdMgmtFolder::SampleSynthDrumUserPresets:
            ok = PerformanceStore::DeleteSampleSynthDrumUserPreset(
                slot, &sample_synth_drum_user_loaded_slot_);
            break;
        default: ok = false; break;
    }

    if(ok)
    {
        snprintf(sd_mgmt_status_, sizeof(sd_mgmt_status_), "Deleted %s%d",
                  SdMgmtFolderShortLabel(sd_mgmt_folder_), slot);
        switch(sd_mgmt_folder_)
        {
            case SdMgmtFolder::Performances: file_slots_dirty_ = true; break;
            case SdMgmtFolder::GranularPresets: granular_preset_slots_dirty_ = true; break;
            case SdMgmtFolder::DexedPresets: dexed_preset_slots_dirty_ = true; break;
            case SdMgmtFolder::EnsemblePresets: ensemble_preset_slots_dirty_ = true; break;
            case SdMgmtFolder::SampleSynthVoiceUserPresets:
                sample_synth_voice_user_dirty_ = true;
                break;
            case SdMgmtFolder::SampleSynthDrumUserPresets:
                sample_synth_drum_user_dirty_ = true;
                break;
            default: break;
        }
        // The list just shrank -- pull the cursor back in range rather
        // than leaving it pointing past the end (SdMgmtSlots() will
        // re-scan on the very next call, so the shrunk count is already
        // current the moment Draw() next asks for it).
        if(sd_mgmt_cursor_ > 0)
            sd_mgmt_cursor_--;
    }
    else
    {
        snprintf(sd_mgmt_status_, sizeof(sd_mgmt_status_), "Fail:%s",
                  PerformanceStore::GetLastError());
    }
}

void Ui::TriggerGranularCaptureFromLayer()
{
    if(!granular_ || !granular_capture_buf_l_ || !granular_capture_buf_r_)
        return;
    if(granular_capture_source_ < 0 || granular_capture_source_ >= num_layers_)
        return;
    int          layer = granular_capture_source_;
    size_t       len   = layers_[layer].GetRecordedLength();
    if(len > granular_capture_capacity_)
        len = granular_capture_capacity_;
    const float* src_l = layers_[layer].GetBufferL();
    const float* src_r = layers_[layer].GetBufferR();
    for(size_t i = 0; i < len; i++)
    {
        granular_capture_buf_l_[i] = src_l[i];
        granular_capture_buf_r_[i] = src_r[i];
    }
    granular_->SetSource(granular_capture_buf_l_, granular_capture_buf_r_, len);
    snprintf(granular_capture_status_, sizeof(granular_capture_status_), "Captured L%d",
              layer + 1);
    OnNewGranularCapture(len);
}

void Ui::RefreshGranularImportFiles()
{
    granular_import_file_count_
        = PerformanceStore::ListImportWavFiles(granular_import_names_, kMaxImportFiles);
    if(granular_import_cursor_ >= granular_import_file_count_)
        granular_import_cursor_ = granular_import_file_count_ > 0 ? granular_import_file_count_ - 1 : 0;
    granular_import_files_dirty_ = false;
}

void Ui::TriggerGranularImport()
{
    if(!granular_ || !granular_capture_buf_l_ || !granular_capture_buf_r_)
        return;
    if(granular_import_cursor_ < 0 || granular_import_cursor_ >= granular_import_file_count_)
        return;

    const char* filename = granular_import_names_[granular_import_cursor_];
    size_t      len       = 0;
    g_progress_disp = disp_;
    bool ok = PerformanceStore::ImportWav(filename, granular_capture_buf_l_,
                                            granular_capture_buf_r_, granular_capture_capacity_,
                                            &len, &Ui::OnSaveLoadProgress);
    g_progress_disp = nullptr;

    if(ok)
    {
        granular_->SetSource(granular_capture_buf_l_, granular_capture_buf_r_, len);
        OnNewGranularCapture(len);
        snprintf(granular_capture_status_, sizeof(granular_capture_status_), "Imported");
    }
    else
    {
        snprintf(granular_capture_status_, sizeof(granular_capture_status_), "Fail:%s",
                  PerformanceStore::GetLastError());
    }
}

void Ui::OnNewGranularCapture(size_t full_len)
{
    granular_capture_full_len_ = full_len;
    granular_trim_start01_     = 0.f;
    granular_trim_end01_       = 1.f;

    // Same downsampled-peak technique GranularEngine::SetSource() uses
    // internally, duplicated here rather than exposed from the engine --
    // this one always covers the FULL capture regardless of what trim is
    // currently active, which is the whole point of keeping it separate.
    for(int col = 0; col < GranularEngine::kWaveformCols; col++)
        granular_trim_full_peaks_[col] = 0.f;
    if(full_len > 0 && granular_capture_buf_l_)
    {
        size_t per_col = full_len / (size_t)GranularEngine::kWaveformCols;
        if(per_col < 1)
            per_col = 1;
        for(int col = 0; col < GranularEngine::kWaveformCols; col++)
        {
            size_t start = (size_t)col * per_col;
            size_t end   = start + per_col;
            if(end > full_len)
                end = full_len;
            float peak = 0.f;
            for(size_t i = start; i < end; i++)
            {
                float a = fabsf(granular_capture_buf_l_[i]);
                if(a > peak)
                    peak = a;
            }
            granular_trim_full_peaks_[col] = peak;
        }
    }
}

void Ui::ApplyGranularTrim()
{
    if(!granular_ || granular_capture_full_len_ == 0)
        return;
    // Minimum gap so the two points can never cross/collapse to nothing
    // audible -- 1% of the full capture, same spirit as this project's
    // other min-range guards (e.g. LooperLayer's own filter cutoff clamp).
    const float kMinGap = 0.01f;
    float       s       = Clampf(granular_trim_start01_, 0.f, 1.f);
    float       e       = Clampf(granular_trim_end01_, 0.f, 1.f);
    if(e < s + kMinGap)
        e = s + kMinGap > 1.f ? 1.f : s + kMinGap;

    size_t full = granular_capture_full_len_;
    size_t start = (size_t)(s * (float)full);
    size_t end   = (size_t)(e * (float)full);
    if(end > full)
        end = full;
    if(end <= start)
        end = start + 1 <= full ? start + 1 : full;

    granular_->SetTrimRange(granular_capture_buf_l_ + start, granular_capture_buf_r_ + start,
                          end - start);

    // Audio range above is already live and correct every tick (cheap,
    // O(1)); the waveform redraw only needs to keep up with what's
    // actually visible, so recompute it far less often than the knob
    // itself is polled -- every 20th call here (~20ms at the main loop's
    // own ~1kHz rate), not every single one. Still feels fully live/
    // continuous to the eye while turning, at a fraction of the cost.
    constexpr uint32_t kPeaksThrottle = 20;
    granular_trim_peaks_throttle_++;
    if(granular_trim_peaks_throttle_ >= kPeaksThrottle)
    {
        granular_trim_peaks_throttle_ = 0;
        granular_->RecomputeWaveformPeaks();
    }
}

bool Ui::IsSampleSynthPageValidForMode(SampleSynthParamPage page, SampleSynthMode mode)
{
    if(mode == SampleSynthMode::Drum)
        // User is shared (same idea as Preset) -- each face has its
        // OWN real User-preset list now (SampleSynthVoiceUserPreset/
        // SampleSynthDrumUserPreset, see those structs' own comments),
        // so Drum's own User page shows/saves Kit-based presets, never
        // Voice's Sound-based ones. Only Capture/Trim/Tune stay Voice-
        // only -- SMPDR has no capture feature at all.
        return page != SampleSynthParamPage::Capture && page != SampleSynthParamPage::Trim
               && page != SampleSynthParamPage::Tune;
    // Voice
    return page != SampleSynthParamPage::Setup;
}

void Ui::SwitchSampleSynthMode(SampleSynthMode target)
{
    if(!sample_synth_ || target == sample_synth_mode_)
        return;
    // Switching faces reloads that face's Kit/Sound, which would reset its
    // unsaved-changes baseline -- keep both as they were.
    const EditSnapshot keep_voice = sample_synth_voice_baseline_;
    const EditSnapshot keep_drum  = sample_synth_drum_baseline_;
    SwitchSampleSynthModeImpl(target);
    sample_synth_voice_baseline_ = keep_voice;
    sample_synth_drum_baseline_  = keep_drum;
}

void Ui::SwitchSampleSynthModeImpl(SampleSynthMode target)
{

    // Snapshot whatever's currently live into the bank for the face
    // being LEFT, so it comes back exactly as left when revisited --
    // see sample_synth_cap_bank_/_imp_bank_/_drum_bank_'s own comments.
    if(sample_synth_mode_ == SampleSynthMode::Drum)
    {
        sample_synth_drum_bank_ = sample_synth_->CapturePreset();
        // Remember the Kit's own current Break Divide/Tune too -- see
        // sample_synth_drum_remembered_slices_'s own comment.
        sample_synth_drum_remembered_slices_     = sample_synth_setup_applied_slices_;
        sample_synth_drum_remembered_tune_semis_ = sample_synth_setup_tune_semis_;
        sample_synth_drum_last_page_             = sample_synth_param_page_;
    }
    else
    {
        if(sample_synth_voice_source_ == SampleSynthVoiceSource::Captured)
            sample_synth_cap_bank_ = sample_synth_->CapturePreset();
        else if(sample_synth_voice_source_ == SampleSynthVoiceSource::Imported)
            sample_synth_imp_bank_ = sample_synth_->CapturePreset();
        sample_synth_voice_last_page_ = sample_synth_param_page_;
    }

    sample_synth_mode_ = target;
    // Restore whichever page THIS face was last left on (Filter by
    // default, the first time it's ever visited) -- NOT a forced
    // reset to Filter on every switch (direct correction: only on
    // startup/first visit, not every time after). See
    // sample_synth_voice_last_page_/_drum_last_page_'s own comment.
    sample_synth_param_page_ = target == SampleSynthMode::Voice
                                    ? sample_synth_voice_last_page_
                                    : sample_synth_drum_last_page_;
    // Preset/Import's own Kits-vs-Sounds split now just follows the
    // face directly -- see that field's own comment.
    sample_synth_preset_mode_sounds_ = (target == SampleSynthMode::Voice);
    sample_synth_kit_cursor_         = 0;
    sample_synth_sound_category_     = -1;
    MarkSampleSynthListsDirty();
    sample_synth_voice_user_dirty_   = true;
    sample_synth_drum_user_dirty_    = true;

    if(target == SampleSynthMode::Drum)
    {
        sample_synth_->ApplyPreset(sample_synth_drum_bank_);
        if(sample_synth_drum_kit_ref_[0] != '\0')
        {
            // Copied first -- LoadSampleSynthKitByRef() rewrites
            // sample_synth_drum_kit_ref_ itself on success.
            char ref[PerformanceStore::kMaxSampleSynthKitRefLen];
            snprintf(ref, sizeof(ref), "%s", sample_synth_drum_kit_ref_);
            LoadSampleSynthKitByRef(ref); // brief SD reload, same as any Kit load
            // LoadSampleSynthKitByRef()'s own OnSampleSynthContentLoaded()
            // call just reset Setup's Break Divide/Tune to off/0 (same
            // as any fresh Kit load) -- re-apply whatever was actually
            // active before this Kit was last left, see
            // sample_synth_drum_remembered_slices_'s own comment.
            sample_synth_setup_pending_slices_ = sample_synth_drum_remembered_slices_;
            sample_synth_setup_tune_semis_     = sample_synth_drum_remembered_tune_semis_;
            ApplySampleSynthBreakDivide(sample_synth_setup_pending_slices_,
                                          sample_synth_setup_tune_semis_);
            sample_synth_setup_applied_slices_ = sample_synth_setup_pending_slices_;
        }
        else
        {
            sample_synth_->ResetAllVoices();
            for(int i = 0; i < SampleSynth::kMaxSampleSlots; i++)
                sample_synth_->ClearSlot(i);
            sample_synth_kit_real_pad_count_ = 0;
        }
    }
    else // entering Voice
    {
        if(sample_synth_voice_source_ == SampleSynthVoiceSource::Captured)
        {
            sample_synth_->ApplyPreset(sample_synth_cap_bank_);
            sample_synth_->SetChromaticMono(sample_synth_cap_mono_);
            // Re-apply slot 0 from the still-resident capture buffer --
            // instant, no SD read, since Drum never touches this
            // separate buffer at all. ApplySampleSynthTrim() also
            // respects whatever Trim range/root_note was last set,
            // rather than reverting to the full untrimmed capture.
            sample_synth_->ResetAllVoices();
            ApplySampleSynthTrim();
        }
        else if(sample_synth_voice_source_ == SampleSynthVoiceSource::Imported)
        {
            sample_synth_->ApplyPreset(sample_synth_imp_bank_);
            sample_synth_->SetChromaticMono(sample_synth_imp_mono_);
            if(sample_synth_voice_sound_name_[0] != '\0') // brief SD reload, same as any Sound load
                LoadSampleSynthFileIntoEngine(sample_synth_voice_sound_category_,
                                                sample_synth_voice_sound_name_);
            for(int si = 0; si < SampleSynth::kMaxSampleSlots; si++)
                if(sample_synth_->SlotHasSample(si))
                    sample_synth_->SetSlotPitchOffsetSemis(si, sample_synth_imp_tune_semis_);
        }
        else
        {
            sample_synth_->ResetAllVoices();
            for(int i = 0; i < SampleSynth::kMaxSampleSlots; i++)
                sample_synth_->ClearSlot(i);
        }
    }
}

void Ui::OnEnterSampleSynthCaptureGroup()
{
    if(!sample_synth_)
        return;
    sample_synth_capture_status_[0] = '\0';
}

void Ui::ClearSampleSynthSlotsForFreshCapture()
{
    if(!sample_synth_)
        return;
    // Was previously run on mere PAGE ENTRY into Capture/Trim/Tune --
    // moved to fire only when a capture/import actually COMPLETES
    // (called right alongside each of the 3 real "slot 0 now holds
    // something new" LoadSlot() calls: Direct Record, From Layer,
    // Import) after a real, reported bug: rotating the encoder the
    // "long way" from Setup to FX passes straight through Capture/
    // Trim/Tune (they're adjacent in the page ring), silently wiping
    // every pad of a loaded Kit down to just slot 0 (C2) even though
    // nothing was actually captured -- exactly "only C2 plays, other
    // notes are silent," with no connection to reverb send at all
    // (the user had simply navigated to the FX page to raise it,
    // passing through this group on the way).
    sample_synth_->ResetAllVoices();
    for(int i = 1; i < SampleSynth::kMaxSampleSlots; i++)
        sample_synth_->ClearSlot(i);
    // No kit survives this wipe -- see sample_synth_kit_real_pad_count_'s
    // own comment (Setup's own Slices has nothing left to re-divide).
    sample_synth_kit_real_pad_count_ = 0;
    OnSampleSynthContentLoaded(0, 0);
    // This IS the capture bank now -- see sample_synth_voice_source_'s
    // own comment. Doesn't touch the Imp bank/remembered Sound name at
    // all, so switching mode away and back still finds it intact if
    // Imp was the more-recently-touched source then.
    sample_synth_voice_source_ = SampleSynthVoiceSource::Captured;
}

void Ui::TriggerSampleSynthCaptureFromLayer()
{
    if(!sample_synth_ || !sample_synth_capture_buf_l_ || !sample_synth_capture_buf_r_)
        return;
    if(sample_synth_capture_source_ < 0 || sample_synth_capture_source_ >= num_layers_)
        return;
    int          layer = sample_synth_capture_source_;
    size_t       len   = layers_[layer].GetRecordedLength();
    if(len > sample_synth_capture_capacity_)
        len = sample_synth_capture_capacity_;
    const float* src_l = layers_[layer].GetBufferL();
    const float* src_r = layers_[layer].GetBufferR();
    for(size_t i = 0; i < len; i++)
    {
        sample_synth_capture_buf_l_[i] = src_l[i];
        sample_synth_capture_buf_r_[i] = src_r[i];
    }
    ClearSampleSynthSlotsForFreshCapture();
    sample_synth_->LoadSlot(0, sample_synth_capture_buf_l_, sample_synth_capture_buf_r_, (int32_t)len,
                              /*chromatic=*/true, sample_synth_tune_root_note_, 0, 127, 0.8f);
    snprintf(sample_synth_capture_status_, sizeof(sample_synth_capture_status_), "Captured L%d",
              layer + 1);
    OnNewSampleSynthCapture(len);
}

void Ui::RefreshSampleSynthImportFiles()
{
    sample_synth_import_file_count_ = PerformanceStore::ListImportWavFiles(
        sample_synth_import_names_, kMaxSampleSynthImportFiles);
    if(sample_synth_import_cursor_ >= sample_synth_import_file_count_)
        sample_synth_import_cursor_
            = sample_synth_import_file_count_ > 0 ? sample_synth_import_file_count_ - 1 : 0;
    sample_synth_import_files_dirty_ = false;
}

void Ui::TriggerSampleSynthImport()
{
    if(!sample_synth_ || !sample_synth_capture_buf_l_ || !sample_synth_capture_buf_r_)
        return;
    if(sample_synth_import_cursor_ < 0
       || sample_synth_import_cursor_ >= sample_synth_import_file_count_)
        return;

    const char* filename = sample_synth_import_names_[sample_synth_import_cursor_];
    size_t      len       = 0;
    // Suspend for the read -- same DMA-contention exposure as
    // TriggerSampleSynthPackImport()'s own ImportWav() calls, see
    // TriggerSave()'s own comment for the confirmed hardware bug.
    g_audio_suspended = true;
    g_progress_disp = disp_;
    bool ok = PerformanceStore::ImportWav(filename, sample_synth_capture_buf_l_,
                                            sample_synth_capture_buf_r_,
                                            sample_synth_capture_capacity_, &len,
                                            &Ui::OnSaveLoadProgress);
    g_progress_disp   = nullptr;
    g_audio_suspended = false;

    if(ok)
    {
        ClearSampleSynthSlotsForFreshCapture();
        sample_synth_->LoadSlot(0, sample_synth_capture_buf_l_, sample_synth_capture_buf_r_,
                                  (int32_t)len, /*chromatic=*/true,
                                  sample_synth_tune_root_note_, 0, 127, 0.8f);
        OnNewSampleSynthCapture(len);
        snprintf(sample_synth_capture_status_, sizeof(sample_synth_capture_status_), "Imported");
    }
    else
    {
        snprintf(sample_synth_capture_status_, sizeof(sample_synth_capture_status_), "Fail:%s",
                  PerformanceStore::GetLastError());
    }
}

void Ui::OnNewSampleSynthCapture(size_t full_len)
{
    sample_synth_capture_unsaved_ = true; // until Trim's own Hold=Save writes it out
    sample_synth_capture_full_len_ = full_len;
    sample_synth_trim_start01_     = 0.f;
    sample_synth_trim_end01_       = 1.f;

    // Same downsampled-peak technique Ui::OnNewGranularCapture() uses --
    // see its own comment for why this is duplicated here rather than
    // exposed from the engine.
    for(int col = 0; col < GranularEngine::kWaveformCols; col++)
        sample_synth_capture_peaks_[col] = 0.f;
    if(full_len > 0 && sample_synth_capture_buf_l_)
    {
        size_t per_col = full_len / (size_t)GranularEngine::kWaveformCols;
        if(per_col < 1)
            per_col = 1;
        for(int col = 0; col < GranularEngine::kWaveformCols; col++)
        {
            size_t start = (size_t)col * per_col;
            size_t end   = start + per_col;
            if(end > full_len)
                end = full_len;
            float peak = 0.f;
            for(size_t i = start; i < end; i++)
            {
                float a = fabsf(sample_synth_capture_buf_l_[i]);
                if(a > peak)
                    peak = a;
            }
            sample_synth_capture_peaks_[col] = peak;
        }
    }
}

void Ui::ComputeSampleSynthTrimRange(size_t* out_start, size_t* out_end) const
{
    // Same 1% minimum-gap guard as Ui::ApplyGranularTrim().
    const float kMinGap = 0.01f;
    float       s       = Clampf(sample_synth_trim_start01_, 0.f, 1.f);
    float       e       = Clampf(sample_synth_trim_end01_, 0.f, 1.f);
    if(e < s + kMinGap)
        e = s + kMinGap > 1.f ? 1.f : s + kMinGap;

    size_t full  = sample_synth_capture_full_len_;
    size_t start = (size_t)(s * (float)full);
    size_t end   = (size_t)(e * (float)full);
    if(end > full)
        end = full;
    if(end <= start)
        end = start + 1 <= full ? start + 1 : full;
    *out_start = start;
    *out_end   = end;
}

void Ui::ApplySampleSynthTrim()
{
    if(!sample_synth_ || sample_synth_capture_full_len_ == 0)
        return;
    size_t start, end;
    ComputeSampleSynthTrimRange(&start, &end);

    sample_synth_->LoadSlot(0, sample_synth_capture_buf_l_ + start,
                              sample_synth_capture_buf_r_ + start, (int32_t)(end - start),
                              /*chromatic=*/true, sample_synth_tune_root_note_, 0, 127,
                              0.8f);
}

void Ui::TriggerSaveSampleSynthCapture()
{
    if(sample_synth_capture_full_len_ == 0)
    {
        snprintf(sample_synth_capture_status_, sizeof(sample_synth_capture_status_),
                  "Nothing to save");
        return;
    }
    size_t start, end;
    ComputeSampleSynthTrimRange(&start, &end);

    char filename[PerformanceStore::kMaxSampleSynthWavNameLen + 1] = {};
    char category[16] = {};
    // Suspend for the write -- same DMA-contention exposure as every
    // other real SD write in this project, see TriggerSave()'s own
    // comment for the confirmed hardware bug this guards against.
    g_audio_suspended = true;
    // Lowest CAPTUREDn with room, a new one created once they're all full.
    int  bank  = PerformanceStore::FindSampleBankWithSpace(PerformanceStore::SampleBank::Captured);
    bool saved = false;
    if(bank > 0)
    {
        PerformanceStore::SampleBankFolder(PerformanceStore::SampleBank::Captured, bank, category,
                                           sizeof(category));
        saved = PerformanceStore::SaveSampleSynthCapture(
            category, sample_synth_capture_buf_l_ + start, sample_synth_capture_buf_r_ + start,
            end - start, filename, sizeof(filename));
    }
    g_audio_suspended = false;
    if(saved)
    {
        snprintf(sample_synth_capture_status_, sizeof(sample_synth_capture_status_), "Saved %s",
                  filename);
        sample_synth_capture_unsaved_ = false;
        MarkSampleSynthListsDirty(); // re-scan so it shows up in the browser immediately
    }
    else
    {
        snprintf(sample_synth_capture_status_, sizeof(sample_synth_capture_status_), "Fail:%s",
                  PerformanceStore::GetLastError());
    }
}

void Ui::TriggerSampleSynthPackImport()
{
    // Wholly separate from SampleSynthParamPage::Capture's own -2
    // Import source -- see that page's own doc comment in ui.h. This
    // sweeps a whole staging folder at once, no Trim/Tune step: a drum
    // pack lands straight as a new Kit, a sampler pack straight as a
    // new Sound, both via PerformanceStore's own importer functions.
    int kits_ok = 0, kits_skip = 0, sounds_ok = 0, sounds_skip = 0;
    g_progress_disp = disp_; // same "WORKING..." overlay every other multi-step SD operation uses
    // Draw the overlay immediately -- the first real file can take a
    // real amount of wall-clock time (AIFF decode + resample + up to
    // 24 slice-file writes), and otherwise the first thing ever drawn
    // was whatever OnSaveLoadProgress()'s own per-file call happened
    // to land on, which for a short batch could be the very last one.
    OnSaveLoadProgress(0.f);

    // Imports every .aif in one staging folder, however many -- listed
    // kMaxSampleSynthImportPackFiles at a time and re-listed until none
    // are left. A file that fails is retried once straight away (some
    // failures are transient -- a file that skipped would import fine
    // on the next press), then left in place for the next press; later
    // passes step past it (ListAifFiles()'s skip_first) so it can never
    // block the files behind it. Success moves the file into Done/.
    // kind: 0 = drum pack (auto Kit/Break), 1 = forced Break, 2 = Sound.
    auto sweep = [&](const char* folder, int kind, int& ok_count, int& skip_count) {
        int stuck = 0; // failed files still sitting in folder, ahead of the rest
        for(;;)
        {
            char names[PerformanceStore::kMaxSampleSynthImportPackFiles]
                      [PerformanceStore::kMaxSampleSynthWavNameLen + 1];
            int count = PerformanceStore::ListAifFiles(
                folder, names, PerformanceStore::kMaxSampleSynthImportPackFiles, stuck);
            if(count == 0)
                break;
            g_audio_suspended = true;
            for(int i = 0; i < count; i++)
            {
                // This file's own share of this pass's bar -- see
                // Ui::OnSampleSynthImportFileProgress()'s own doc comment.
                g_progress_file_base01 = (float)i / (float)count;
                g_progress_file_span01 = 1.f / (float)count;
                bool ok = false;
                for(int attempt = 0; attempt < 2 && !ok; attempt++)
                {
                    char status[48];
                    ok = kind == 2
                             ? PerformanceStore::ImportAifSamplerPack(
                                 names[i], folder, sample_synth_arena_l_, sample_synth_arena_r_,
                                 sample_synth_arena_capacity_, status, sizeof(status))
                             : PerformanceStore::ImportAifDrumPack(
                                 names[i], folder, kind == 1, sample_synth_arena_l_,
                                 sample_synth_arena_r_, sample_synth_arena_capacity_, status,
                                 sizeof(status), &Ui::OnSampleSynthImportFileProgress);
                }
                if(ok)
                    ok_count++;
                else
                    skip_count++;
                // A file that stays put (failed, or imported but couldn't
                // be moved) must be stepped past on the next pass.
                if(!ok || !PerformanceStore::MoveToImportDoneFolder(folder, names[i]))
                    stuck++;
                OnSaveLoadProgress((float)(i + 1) / (float)count);
            }
            g_audio_suspended = false;
        }
    };

    if(sample_synth_import_target_ == SampleSynthImportTarget::Kit
       || sample_synth_import_target_ == SampleSynthImportTarget::All)
    {
        sweep(PerformanceStore::kSampleKitImportFolder, 0, kits_ok, kits_skip);
        sweep(PerformanceStore::kSampleBreakImportFolder, 1, kits_ok, kits_skip);
    }
    if(sample_synth_import_target_ == SampleSynthImportTarget::Sound
       || sample_synth_import_target_ == SampleSynthImportTarget::All)
        sweep(PerformanceStore::kSampleSoundImportFolder, 2, sounds_ok, sounds_skip);
    if(kits_ok > 0 || sounds_ok > 0)
        MarkSampleSynthListsDirty();

    g_progress_disp = nullptr;

    if(sample_synth_import_target_ == SampleSynthImportTarget::Kit)
        snprintf(sample_synth_pack_import_status_, sizeof(sample_synth_pack_import_status_),
                  "Drums: %d ok, %d skip", kits_ok, kits_skip);
    else if(sample_synth_import_target_ == SampleSynthImportTarget::Sound)
        snprintf(sample_synth_pack_import_status_, sizeof(sample_synth_pack_import_status_),
                  "Sounds: %d ok, %d skip", sounds_ok, sounds_skip);
    else
        snprintf(sample_synth_pack_import_status_, sizeof(sample_synth_pack_import_status_),
                  "Drm %d/%d Snd %d/%d", kits_ok, kits_skip, sounds_ok, sounds_skip);
}

void Ui::SampleBankEntryFolder(const SampleBankEntry& e, char* out, size_t out_size)
{
    PerformanceStore::SampleBankFolder(e.type, e.number, out, out_size);
}

void Ui::SampleBankEntryLabel(const SampleBankEntry& e, char* out, size_t out_size)
{
    const char* name = "Sounds";
    switch(e.type)
    {
        case PerformanceStore::SampleBank::Sounds: name = "Sounds"; break;
        case PerformanceStore::SampleBank::Captured: name = "Captured"; break;
        case PerformanceStore::SampleBank::Kits: name = "Kits"; break;
        case PerformanceStore::SampleBank::Breaks: name = "Breaks"; break;
    }
    snprintf(out, out_size, "%s %d", name, e.number);
}

int Ui::ListFaceSampleBanks(bool drum, SampleBankEntry* out, int max_out)
{
    using PerformanceStore::SampleBank;
    const SampleBank types[2] = {drum ? SampleBank::Kits : SampleBank::Sounds,
                                 drum ? SampleBank::Breaks : SampleBank::Captured};
    int count = 0;
    for(SampleBank t : types)
    {
        uint8_t numbers[PerformanceStore::kMaxSampleBanks];
        int     n = PerformanceStore::ListSampleBanks(t, numbers, PerformanceStore::kMaxSampleBanks);
        for(int i = 0; i < n && count < max_out; i++)
        {
            out[count].type   = t;
            out[count].number = numbers[i];
            count++;
        }
    }
    return count;
}

void Ui::MarkSampleSynthListsDirty()
{
    sample_synth_banks_dirty_  = true;
    sample_synth_sounds_dirty_ = true;
    sd_mgmt_banks_dirty_       = true;
    sd_mgmt_items_dirty_       = true;
}

void Ui::TriggerLoadSampleSynthKit()
{
    if(!sample_synth_ || sample_synth_sound_category_ < 0
       || sample_synth_sound_category_ >= sample_synth_bank_count_ || sample_synth_kit_cursor_ < 0
       || sample_synth_kit_cursor_ >= sample_synth_sound_count_)
        return;
    char folder[16];
    SampleBankEntryFolder(sample_synth_banks_[sample_synth_sound_category_], folder,
                          sizeof(folder));
    char ref[PerformanceStore::kMaxSampleSynthKitRefLen];
    snprintf(ref, sizeof(ref), "%s/%s", folder, sample_synth_sound_names_[sample_synth_kit_cursor_]);
    LoadSampleSynthKitByRef(ref);
}

bool Ui::LoadSampleSynthKitByRef(const char* kit_ref)
{
    if(!sample_synth_ || !kit_ref || kit_ref[0] == '\0')
        return false;
    // Kit pad filenames are relative to the kit's own folder.
    char kit_dir[64];
    PerformanceStore::SampleSynthKitDir(kit_ref, kit_dir, sizeof(kit_dir));

    char                                      kit_name[PerformanceStore::kMaxSampleSynthKitNameLen + 1];
    SampleSynth::SampleSynthPresetData       params;
    PerformanceStore::SampleSynthKitSlot slots[PerformanceStore::kMaxSampleSynthSlots];

    // Same full g_audio_suspended guard every other load/save function
    // in this file uses (see TriggerLoadGranularPreset()'s own comment
    // for why a narrower fix was tried and disproven on real hardware)
    // -- this one's busier than most (up to kMaxSampleSynthSlots
    // separate f_open()/f_read() sequences, one per referenced WAV, not
    // just one), so it stays suspended for the whole loop below, not
    // released between files.
    g_audio_suspended = true;
    bool ok = PerformanceStore::LoadSampleSynthKit(kit_ref, kit_name, sizeof(kit_name), &params,
                                                     slots);
    if(ok)
    {
        // Kill every voice before any slot gets reassigned -- a voice
        // still sounding (held note, or an unfinished release tail)
        // keeps referencing its slot by INDEX, not content, so
        // swapping that slot's buffer out from under it would make it
        // immediately start playing the new audio with no NoteOn at
        // all. See SampleSynth::ResetAllVoices()'s own doc comment.
        sample_synth_->ResetAllVoices();
        sample_synth_->ApplyPreset(params);
        // Bump-allocate each referenced file into the next free stretch
        // of the shared arena -- see main.cpp's own g_sample_synth_arena_l
        // doc comment. Resets to the start every load (a new kit always
        // replaces whatever the previous one filled, same "one thing
        // resident at a time" convention Grains' own single capture
        // buffer already uses).
        size_t cursor       = 0;
        int    loaded_count = 0;
        for(int i = 0; i < PerformanceStore::kMaxSampleSynthSlots; i++)
        {
            if(slots[i].filename[0] == '\0')
            {
                sample_synth_->ClearSlot(i);
                continue;
            }
            if(cursor >= sample_synth_arena_capacity_)
            {
                sample_synth_->ClearSlot(i); // arena's full -- skip, don't overrun
                continue;
            }
            size_t len = 0;
            bool   file_ok
                = PerformanceStore::ImportWav(slots[i].filename, sample_synth_arena_l_ + cursor,
                                                sample_synth_arena_r_ + cursor,
                                                sample_synth_arena_capacity_ - cursor, &len,
                                                nullptr, kit_dir);
            if(!file_ok || len == 0)
            {
                sample_synth_->ClearSlot(i);
                continue;
            }
            sample_synth_->LoadSlot(i, sample_synth_arena_l_ + cursor,
                                      sample_synth_arena_r_ + cursor, (int32_t)len,
                                      slots[i].chromatic, slots[i].root_note, slots[i].low_note,
                                      slots[i].high_note, slots[i].volume01);
            // Cache this real pad -- compacted, load order -- so Setup's
            // own Break Divide can always recompute fresh from the
            // ORIGINAL layout later, however many times it's toggled.
            // See sample_synth_kit_real_slots_'s own comment.
            if(loaded_count < PerformanceStore::kMaxSampleSynthSlots)
            {
                sample_synth_kit_real_slots_[loaded_count]    = slots[i];
                sample_synth_kit_real_pad_len_[loaded_count] = (int32_t)len;
            }
            cursor += len;
            loaded_count++;
        }
        snprintf(sample_synth_loaded_kit_ref_, sizeof(sample_synth_loaded_kit_ref_), "%s", kit_ref);
        // Drum's own "what to restore" -- see its own comment.
        snprintf(sample_synth_drum_kit_ref_, sizeof(sample_synth_drum_kit_ref_), "%s", kit_ref);
        sample_synth_loaded_sound_name_[0]   = '\0'; // a kit replaced whatever single sound was loaded
        sample_synth_loaded_sound_category_[0] = '\0';
        sample_synth_kit_real_pad_count_ = loaded_count;
        sample_synth_kit_real_total_len_ = cursor;
        snprintf(sample_synth_loaded_kit_name_, sizeof(sample_synth_loaded_kit_name_), "%s",
                  kit_name);
        snprintf(sample_synth_kit_status_, sizeof(sample_synth_kit_status_), "Loaded (%d)",
                  loaded_count);
        // Setup's own Break Divide/Tune are pure live/session state,
        // not saved with the kit (there's no user-facing Save for Kits
        // at all -- see SampleSynthParamPage::Preset's own doc
        // comment) -- a fresh load always starts at off/0, same
        // "doesn't leak from whatever was loaded before" reset
        // mono_choke/Pan/Level already get via ApplyPreset() above.
        OnSampleSynthContentLoaded(0, 0);
    }
    else
    {
        snprintf(sample_synth_kit_status_, sizeof(sample_synth_kit_status_), "Fail:%s",
                  PerformanceStore::GetLastError());
    }
    g_audio_suspended = false;
    return ok;
}

void Ui::OnSampleSynthContentLoaded(int applied_slices, int8_t tune_semis)
{
    sample_synth_setup_tune_semis_    = tune_semis;
    sample_synth_setup_applied_slices_ = applied_slices;
    // Only seed the pending knob position when nothing's actually
    // configured -- otherwise re-entering Setup on a kit that already
    // has a real Break Divide shows what's really active, not a reset
    // suggestion (see sample_synth_setup_pending_slices_'s own comment).
    sample_synth_setup_pending_slices_ = applied_slices > 0 ? applied_slices : 16;
    MarkSampleSynthClean(); // a freshly loaded Kit/Sound has nothing unsaved yet
}

void Ui::ApplySampleSynthBreakDivide(int divide_count, int8_t tune_semis)
{
    if(!sample_synth_)
        return;
    int pad_count = sample_synth_kit_real_pad_count_;
    if(pad_count <= 0)
        return; // a single Sound is loaded, or no kit -- nothing to re-divide
    if(divide_count < 0)
        divide_count = 0;
    if(divide_count > 16)
        divide_count = 16;

    // Kill every voice before any slot gets reassigned -- same
    // reasoning/precedent as TriggerLoadSampleSynthKit()'s own
    // ResetAllVoices() call: a voice still sounding (held note, or
    // mid release-tail) keeps referencing its slot by INDEX, not
    // content, so remapping that slot out from under it mid-playback
    // would make it briefly render from whatever new audio now lives
    // at that index until it next retriggers (NoteOn() always
    // re-resolves a fresh slot per call, so this is a transient
    // glitch, not a lasting one) -- cheap enough to just avoid
    // entirely, same as the Kit-load case already does.
    sample_synth_->ResetAllVoices();

    // C2 -- same base key this project's own OP-1 drum-pack import
    // already uses for its own 24-pad span (ImportAifDrumPack()).
    constexpr uint8_t kBaseKey = 36;

    if(divide_count == 0)
    {
        // Restore the kit's real, originally-loaded per-pad mapping --
        // same offsets the initial load produced, recomputed fresh
        // from the cache rather than trusting whatever's currently in
        // slots_[] (which might be a previous, different Break Divide).
        size_t offset = 0;
        for(int j = 0; j < pad_count; j++)
        {
            const PerformanceStore::SampleSynthKitSlot& s = sample_synth_kit_real_slots_[j];
            int32_t len                                    = sample_synth_kit_real_pad_len_[j];
            sample_synth_->LoadSlot(j, sample_synth_arena_l_ + offset,
                                      sample_synth_arena_r_ + offset, len, s.chromatic,
                                      s.root_note, s.low_note, s.high_note, s.volume01,
                                      tune_semis);
            offset += (size_t)len;
        }
    }
    else
    {
        // Re-chop the kit's ENTIRE combined span (every real pad,
        // back-to-back in the arena -- see this feature's own
        // DESIGN.md writeup for why that's already true in RAM with no
        // extra work) into divide_count equal-length one-shot pieces on
        // consecutive keys from kBaseKey.
        size_t total = sample_synth_kit_real_total_len_;
        for(int j = 0; j < divide_count; j++)
        {
            size_t start = (size_t)((uint64_t)total * (uint64_t)j / (uint64_t)divide_count);
            size_t end = (size_t)((uint64_t)total * (uint64_t)(j + 1) / (uint64_t)divide_count);
            sample_synth_->LoadSlot(j, sample_synth_arena_l_ + start, sample_synth_arena_r_ + start,
                                      (int32_t)(end - start), /*chromatic=*/false,
                                      /*root_note=*/kBaseKey, (uint8_t)(kBaseKey + j),
                                      (uint8_t)(kBaseKey + j), /*volume01=*/1.f, tune_semis);
        }
    }
    // Sweep every slot beyond what was just (re)loaded, all the way to
    // the engine's own real ceiling -- not just up to max(pad_count,
    // divide_count). A PREVIOUS, larger split (e.g. 16, confirmed
    // earlier) can have populated slots well past whatever this call
    // actually touches (e.g. re-splitting down to 8 only loads
    // [0,8)) -- stopping the sweep early left slots 8-15 holding their
    // old 16-way content, a real reported bug ("remaining 8 play the
    // slots from when it was split to 16"). ClearSlot() is cheap, so
    // there's no reason to bound this any tighter than the real max.
    for(int j = (divide_count == 0 ? pad_count : divide_count); j < SampleSynth::kMaxSampleSlots;
        j++)
        sample_synth_->ClearSlot(j);
}

void Ui::TriggerSampleSynthBreakDivideConfirm()
{
    ApplySampleSynthBreakDivide(sample_synth_setup_pending_slices_, sample_synth_setup_tune_semis_);
    sample_synth_setup_applied_slices_ = sample_synth_kit_real_pad_count_ > 0
                                              ? sample_synth_setup_pending_slices_
                                              : 0; // no-op on a loaded Sound -- see that setter
}

namespace
{
// ".snd" vs plain ".wav" -- see PerformanceStore::LoadSampleSynthSound()'s
// own doc comment for what a .snd manifest is.
bool HasExtension(const char* filename, const char* ext_lower)
{
    size_t flen = strlen(filename);
    size_t elen = strlen(ext_lower);
    if(flen < elen)
        return false;
    for(size_t i = 0; i < elen; i++)
        if((char)tolower((unsigned char)filename[flen - elen + i]) != ext_lower[i])
            return false;
    return true;
}
} // namespace

// Shared by TriggerLoadSampleSynthSound() (category_folder comes from
// sample_synth_sound_category_, the category currently being browsed),
// TriggerLoadSampleSynthVoiceUserPreset() (comes from the saved User
// preset's own reference), and SwitchSampleSynthMode()'s own Voice-
// restore path (comes from sample_synth_voice_sound_name_/_category_)
// -- the actual .snd-vs-.wav load logic is identical either way, only
// where the (category, filename) pair comes from differs.
void Ui::LoadSampleSynthFileIntoEngine(const char* category_folder, const char* filename)
{
    if(!sample_synth_ || !category_folder || !filename || filename[0] == '\0')
        return;

    // Current ADSR/filter/output/pan/send settings are deliberately
    // left untouched -- see sample_synth_preset_mode_sounds_'s own doc
    // comment for why this is "audition a sound," not "recall a
    // configuration" the way a kit load is.
    g_audio_suspended = true;
    // Kill every voice before any slot gets reassigned -- see
    // SampleSynth::ResetAllVoices()'s own doc comment for why (a voice
    // still sounding from before would otherwise jump straight to the
    // new audio with no NoteOn at all).
    sample_synth_->ResetAllVoices();

    if(HasExtension(filename, ".snd"))
    {
        // Multi-sample melodic instrument -- several recordings, each
        // its own zone, so every note plays from its nearest real
        // pitch instead of one sample stretched across the whole
        // keyboard. See PerformanceStore::LoadSampleSynthSound()'s own
        // doc comment. Bump-allocates each zone's WAV into the shared
        // arena exactly like TriggerLoadSampleSynthKit() already does.
        // The .snd manifest itself lives in this category's own
        // subfolder, but the zone WAVs it references (e.g.
        // "DRUMS/SKOGSFIOL C4.wav") are their own relative paths from
        // SAMPLES/ -- unrelated to the category -- so only the
        // manifest lookup itself is category-scoped, the per-zone
        // ImportWav() calls below stay as they were.
        PerformanceStore::SampleSynthKitSlot slots[PerformanceStore::kMaxSampleSynthSlots];
        bool ok = PerformanceStore::LoadSampleSynthSound(filename, slots, category_folder);
        if(ok)
        {
            size_t cursor       = 0;
            int    loaded_count = 0;
            for(int i = 0; i < PerformanceStore::kMaxSampleSynthSlots; i++)
            {
                if(slots[i].filename[0] == '\0')
                {
                    sample_synth_->ClearSlot(i);
                    continue;
                }
                if(cursor >= sample_synth_arena_capacity_)
                {
                    sample_synth_->ClearSlot(i); // arena's full -- skip, don't overrun
                    continue;
                }
                size_t len = 0;
                bool   file_ok = PerformanceStore::ImportWav(
                    slots[i].filename, sample_synth_arena_l_ + cursor,
                    sample_synth_arena_r_ + cursor, sample_synth_arena_capacity_ - cursor, &len,
                    nullptr, PerformanceStore::kSampleSynthFolder);
                if(!file_ok || len == 0)
                {
                    sample_synth_->ClearSlot(i);
                    continue;
                }
                sample_synth_->LoadSlot(i, sample_synth_arena_l_ + cursor,
                                          sample_synth_arena_r_ + cursor, (int32_t)len,
                                          slots[i].chromatic, slots[i].root_note,
                                          slots[i].low_note, slots[i].high_note,
                                          slots[i].volume01);
                cursor += len;
                loaded_count++;
            }
            sample_synth_loaded_kit_ref_[0] = '\0'; // no kit is loaded any more
            sample_synth_loaded_kit_name_[0] = '\0';
            snprintf(sample_synth_loaded_sound_name_, sizeof(sample_synth_loaded_sound_name_),
                      "%s", filename);
            snprintf(sample_synth_loaded_sound_category_,
                      sizeof(sample_synth_loaded_sound_category_), "%s", category_folder);
            snprintf(sample_synth_kit_status_, sizeof(sample_synth_kit_status_), "Loaded (%d)",
                      loaded_count);
            // A Sound, not a Kit -- Setup's own Slices has nothing to
            // re-divide (see sample_synth_kit_real_pad_count_'s own
            // comment), and Tune resets fresh for whatever just loaded.
            sample_synth_kit_real_pad_count_ = 0;
            OnSampleSynthContentLoaded(0, 0);
            // This IS the Imp bank's source now -- see
            // sample_synth_voice_source_'s own comment. Remembered
            // separately from sample_synth_loaded_sound_name_/
            // _category_ above (which Drum-mode Kit loads can stomp)
            // so SwitchSampleSynthMode() always knows what to reload.
            sample_synth_voice_source_ = SampleSynthVoiceSource::Imported;
            snprintf(sample_synth_voice_sound_name_, sizeof(sample_synth_voice_sound_name_), "%s",
                      filename);
            snprintf(sample_synth_voice_sound_category_,
                      sizeof(sample_synth_voice_sound_category_), "%s", category_folder);
        }
        else
        {
            snprintf(sample_synth_kit_status_, sizeof(sample_synth_kit_status_), "Fail:%s",
                      PerformanceStore::GetLastError());
        }
        g_audio_suspended = false;
        return;
    }

    char wav_folder[40];
    snprintf(wav_folder, sizeof(wav_folder), "%s/%s", PerformanceStore::kSampleSynthFolder,
              category_folder);
    size_t len = 0;
    bool   ok  = PerformanceStore::ImportWav(filename, sample_synth_arena_l_,
                                               sample_synth_arena_r_, sample_synth_arena_capacity_,
                                               &len, nullptr, wav_folder);
    if(ok && len > 0)
    {
        for(int i = 0; i < SampleSynth::kMaxSampleSlots; i++)
            sample_synth_->ClearSlot(i);
        // Respect a pitch stated right in the filename (e.g. "SKOGSFIOL
        // C4.wav") instead of always assuming C4 -- see
        // ParseNoteNameFromFilename()'s own comment. Falls back to C4
        // only for files that never claimed a pitch in the first place.
        uint8_t root_note = 60;
        PerformanceStore::ParseNoteNameFromFilename(filename, &root_note);
        sample_synth_->LoadSlot(0, sample_synth_arena_l_, sample_synth_arena_r_, (int32_t)len,
                                  /*chromatic=*/true, root_note, /*low_note=*/0,
                                  /*high_note=*/127, /*volume01=*/0.8f);
        sample_synth_loaded_kit_ref_[0] = '\0'; // no kit is loaded any more
        sample_synth_loaded_kit_name_[0] = '\0';
        snprintf(sample_synth_loaded_sound_name_, sizeof(sample_synth_loaded_sound_name_), "%s",
                  filename);
        snprintf(sample_synth_loaded_sound_category_,
                  sizeof(sample_synth_loaded_sound_category_), "%s", category_folder);
        snprintf(sample_synth_kit_status_, sizeof(sample_synth_kit_status_), "Loaded");
        sample_synth_kit_real_pad_count_ = 0; // a Sound, not a Kit -- see above
        OnSampleSynthContentLoaded(0, 0);
        sample_synth_voice_source_ = SampleSynthVoiceSource::Imported;
        snprintf(sample_synth_voice_sound_name_, sizeof(sample_synth_voice_sound_name_), "%s",
                  filename);
        snprintf(sample_synth_voice_sound_category_, sizeof(sample_synth_voice_sound_category_),
                  "%s", category_folder);
    }
    else
    {
        snprintf(sample_synth_kit_status_, sizeof(sample_synth_kit_status_), "Fail:%s",
                  PerformanceStore::GetLastError());
    }
    g_audio_suspended = false;
}

void Ui::TriggerLoadSampleSynthSound()
{
    if(sample_synth_kit_cursor_ < 0 || sample_synth_kit_cursor_ >= sample_synth_sound_count_
       || sample_synth_sound_category_ < 0 || sample_synth_sound_category_ >= sample_synth_bank_count_)
        return;
    char category_folder[16];
    SampleBankEntryFolder(sample_synth_banks_[sample_synth_sound_category_], category_folder,
                          sizeof(category_folder));
    sample_synth_capture_unsaved_ = false; // the capture is being replaced
    LoadSampleSynthFileIntoEngine(category_folder,
                                    sample_synth_sound_names_[sample_synth_kit_cursor_]);
}

void Ui::TriggerSaveSampleSynthVoiceUserPreset(bool force_new)
{
    if(!sample_synth_)
        return;
    // Needs a real file reference -- a live-but-unsaved Capture has no
    // filename to save alongside (see SampleSynthVoiceUserPreset's own
    // comment). Trim's own "hold B2 = save as Sound" turns a capture
    // into a real file first, which can then be User-saved too.
    if(sample_synth_voice_source_ != SampleSynthVoiceSource::Imported
       || sample_synth_voice_sound_name_[0] == '\0')
    {
        snprintf(sample_synth_voice_user_status_, sizeof(sample_synth_voice_user_status_),
                  "Import a Sound first");
        return;
    }
    int slot = (!force_new && sample_synth_voice_user_loaded_slot_ > 0)
                   ? sample_synth_voice_user_loaded_slot_
                   : PerformanceStore::NextFreeSampleSynthVoiceUserPresetSlot();
    if(slot < 0)
    {
        snprintf(sample_synth_voice_user_status_, sizeof(sample_synth_voice_user_status_),
                  "Card full/missing");
        return;
    }

    PerformanceStore::SampleSynthVoiceUserPreset preset;
    snprintf(preset.category, sizeof(preset.category), "%s", sample_synth_voice_sound_category_);
    snprintf(preset.filename, sizeof(preset.filename), "%s", sample_synth_voice_sound_name_);
    preset.params     = sample_synth_->CapturePreset();
    preset.tune_semis = sample_synth_imp_tune_semis_;
    preset.mono       = sample_synth_imp_mono_;

    g_audio_suspended = true;
    bool ok           = PerformanceStore::SaveSampleSynthVoiceUserPreset(slot, preset);
    g_audio_suspended = false;
    if(ok)
    {
        sample_synth_voice_user_loaded_slot_ = slot;
        snprintf(sample_synth_voice_user_status_, sizeof(sample_synth_voice_user_status_),
                  "Saved %d", slot);
        MarkSampleSynthClean();
        sample_synth_voice_user_dirty_ = true; // a new slot may now exist
    }
    else
    {
        snprintf(sample_synth_voice_user_status_, sizeof(sample_synth_voice_user_status_),
                  "Fail:%s", PerformanceStore::GetLastError());
    }
}

void Ui::TriggerLoadSampleSynthVoiceUserPreset()
{
    if(!sample_synth_ || sample_synth_voice_user_cursor_ < 0
       || sample_synth_voice_user_cursor_ >= sample_synth_voice_user_count_)
        return;
    int slot = sample_synth_voice_user_numbers_[sample_synth_voice_user_cursor_];

    PerformanceStore::SampleSynthVoiceUserPreset preset;
    bool ok = PerformanceStore::LoadSampleSynthVoiceUserPreset(slot, &preset);
    if(!ok)
    {
        snprintf(sample_synth_voice_user_status_, sizeof(sample_synth_voice_user_status_),
                  "Fail:%s", PerformanceStore::GetLastError());
        return;
    }
    // Load the referenced Sound first (sets sample_synth_voice_source_/
    // _sound_name_/_category_ correctly on its own), THEN apply the
    // saved settings on top -- same order Tune's own K2/B2 handlers
    // and SwitchSampleSynthMode()'s own Voice-restore path already use.
    // Saved before Sounds moved into numbered banks: plain "SOUNDS" is
    // what's now SOUNDS1.
    if(strcmp(preset.category, "SOUNDS") == 0)
        snprintf(preset.category, sizeof(preset.category), "SOUNDS1");
    sample_synth_capture_unsaved_ = false; // the capture is being replaced
    LoadSampleSynthFileIntoEngine(preset.category, preset.filename);
    sample_synth_->ApplyPreset(preset.params);
    sample_synth_->SetChromaticMono(preset.mono);
    sample_synth_imp_tune_semis_ = preset.tune_semis;
    sample_synth_imp_mono_       = preset.mono;
    for(int si = 0; si < SampleSynth::kMaxSampleSlots; si++)
        if(sample_synth_->SlotHasSample(si))
            sample_synth_->SetSlotPitchOffsetSemis(si, preset.tune_semis);
    sample_synth_voice_user_loaded_slot_ = slot;
    snprintf(sample_synth_voice_user_status_, sizeof(sample_synth_voice_user_status_),
              "Loaded SMPVO%d", slot);
    MarkSampleSynthClean();
}

void Ui::TriggerSaveSampleSynthDrumUserPreset(bool force_new)
{
    if(!sample_synth_)
        return;
    if(sample_synth_drum_kit_ref_[0] == '\0')
    {
        snprintf(sample_synth_drum_user_status_, sizeof(sample_synth_drum_user_status_),
                  "Load a Kit first");
        return;
    }
    int slot = (!force_new && sample_synth_drum_user_loaded_slot_ > 0)
                   ? sample_synth_drum_user_loaded_slot_
                   : PerformanceStore::NextFreeSampleSynthDrumUserPresetSlot();
    if(slot < 0)
    {
        snprintf(sample_synth_drum_user_status_, sizeof(sample_synth_drum_user_status_),
                  "Card full/missing");
        return;
    }

    PerformanceStore::SampleSynthDrumUserPreset preset;
    snprintf(preset.kit_ref, sizeof(preset.kit_ref), "%s", sample_synth_drum_kit_ref_);
    preset.params             = sample_synth_->CapturePreset();
    preset.break_divide_count = (uint8_t)sample_synth_setup_applied_slices_;
    preset.break_tune_semis   = sample_synth_setup_tune_semis_;

    g_audio_suspended = true;
    bool ok           = PerformanceStore::SaveSampleSynthDrumUserPreset(slot, preset);
    g_audio_suspended = false;
    if(ok)
    {
        sample_synth_drum_user_loaded_slot_ = slot;
        snprintf(sample_synth_drum_user_status_, sizeof(sample_synth_drum_user_status_), "Saved %d",
                  slot);
        MarkSampleSynthClean();
        sample_synth_drum_user_dirty_ = true;
    }
    else
    {
        snprintf(sample_synth_drum_user_status_, sizeof(sample_synth_drum_user_status_), "Fail:%s",
                  PerformanceStore::GetLastError());
    }
}

void Ui::TriggerLoadSampleSynthDrumUserPreset()
{
    if(!sample_synth_ || sample_synth_drum_user_cursor_ < 0
       || sample_synth_drum_user_cursor_ >= sample_synth_drum_user_count_)
        return;
    int slot = sample_synth_drum_user_numbers_[sample_synth_drum_user_cursor_];

    PerformanceStore::SampleSynthDrumUserPreset preset;
    bool ok = PerformanceStore::LoadSampleSynthDrumUserPreset(slot, &preset);
    if(!ok)
    {
        snprintf(sample_synth_drum_user_status_, sizeof(sample_synth_drum_user_status_), "Fail:%s",
                  PerformanceStore::GetLastError());
        return;
    }
    if(!LoadSampleSynthKitByRef(preset.kit_ref))
    {
        snprintf(sample_synth_drum_user_status_, sizeof(sample_synth_drum_user_status_),
                  "Kit missing");
        return;
    }
    sample_synth_->ApplyPreset(preset.params);
    sample_synth_setup_pending_slices_ = preset.break_divide_count;
    sample_synth_setup_tune_semis_     = preset.break_tune_semis;
    ApplySampleSynthBreakDivide(sample_synth_setup_pending_slices_, sample_synth_setup_tune_semis_);
    sample_synth_setup_applied_slices_ = sample_synth_setup_pending_slices_;
    // Keep the mode-switch "remember what to restore" fields in sync
    // with what's now actually active -- see
    // sample_synth_drum_remembered_slices_'s own comment.
    sample_synth_drum_remembered_slices_     = sample_synth_setup_applied_slices_;
    sample_synth_drum_remembered_tune_semis_ = sample_synth_setup_tune_semis_;
    sample_synth_drum_user_loaded_slot_       = slot;
    snprintf(sample_synth_drum_user_status_, sizeof(sample_synth_drum_user_status_),
              "Loaded SMPDR%d", slot);
    MarkSampleSynthClean();
}

void Ui::RefreshGranularPresetSlots()
{
    granular_preset_user_slot_count_ = PerformanceStore::ListGranularPresets(
        granular_preset_user_slots_, kMaxGranularPresetSlots);
    if(granular_preset_cursor_ >= granular_preset_user_slot_count_)
        granular_preset_cursor_
            = granular_preset_user_slot_count_ > 0 ? granular_preset_user_slot_count_ - 1 : 0;
    granular_preset_slots_dirty_ = false;
}

void Ui::TriggerSaveGranularPreset(bool force_new)
{
    if(!granular_)
        return;
    // Smart save (force_new=false): overwrite granular_loaded_preset_slot_
    // if it names a real slot; nothing loaded (-1) has no slot to
    // overwrite, so fall back to a new one -- same reasoning TriggerSave()
    // uses for its own loaded-slot tracking.
    int slot = (!force_new && granular_loaded_preset_slot_ > 0)
                   ? granular_loaded_preset_slot_
                   : PerformanceStore::NextFreeGranularPresetSlot();
    if(slot < 0)
    {
        snprintf(granular_preset_status_, sizeof(granular_preset_status_), "Card full/missing");
        return;
    }

    GranularEngine::GranularPresetData preset = granular_->CapturePreset();

    // See the g_audio_suspended comment in TriggerSave() above -- same
    // long blocking SD write, same intermittent "Fail:close9"
    // (FR_INVALID_OBJECT) risk from the full-cost audio ISR contending
    // with the SDMMC DMA transfer for the whole write duration.
    g_audio_suspended = true;
    g_progress_disp   = disp_;
    bool ok = PerformanceStore::SaveGranularPreset(
        slot, preset, granular_->GetSourceL(), granular_->GetSourceR(), granular_->GetSourceLen(),
        &Ui::OnSaveLoadProgress);
    g_progress_disp   = nullptr;
    g_audio_suspended = false;

    if(ok)
    {
        granular_loaded_preset_slot_ = slot;
        snprintf(granular_preset_status_, sizeof(granular_preset_status_), "Saved %d", slot);
        granular_preset_slots_dirty_ = true; // a new slot may now exist
    }
    else
    {
        snprintf(granular_preset_status_, sizeof(granular_preset_status_), "Fail:%s",
                  PerformanceStore::GetLastError());
    }
}

void Ui::TriggerNewGranularPreset()
{
    if(!granular_)
        return;
    // The Load chooser's "New" pick (see ApplyKnobs()'s BrowsingLoad
    // handling) -- clears the captured audio and resets every param to
    // the engine's own defaults, same fresh-start spirit as Global:File's
    // own TriggerNew().
    granular_->ApplyPreset(GranularEngine::GranularPresetData{});
    granular_->SetSource(granular_capture_buf_l_, granular_capture_buf_r_, 0);
    OnNewGranularCapture(0);
    granular_loaded_preset_slot_ = 0;
    snprintf(granular_preset_status_, sizeof(granular_preset_status_), "New");
}

void Ui::TriggerLoadGranularPreset()
{
    if(!granular_)
        return;
    // !load_browsing_files_ means the top-level chooser confirmed with
    // "New" highlighted (can_confirm_load only allows a confirm here when
    // that's the case -- see HandleButton2()).
    if(!load_browsing_files_)
    {
        TriggerNewGranularPreset();
        return;
    }
    if(granular_preset_cursor_ < 0
       || granular_preset_cursor_ >= granular_preset_user_slot_count_)
        return;
    int slot = granular_preset_user_slots_[granular_preset_cursor_];

    GranularEngine::GranularPresetData preset;
    size_t audio_len = 0;
    // Tried detaching just the granular engine from the buffer being
    // overwritten (SetSource(nullptr, nullptr, 0) instead of a full
    // g_audio_suspended) on the theory that the real race was Process()
    // reading src_l_/src_r_ while the SD DMA transfer rewrote them --
    // real hardware testing disproved that: the failure (AUD0:1,
    // FR_DISK_ERR) persisted identically with the engine detached. That
    // rules out a buffer-specific race and points at the same general
    // audio-ISR-vs-SDMMC-DMA contention the original "close9"
    // (FR_INVALID_OBJECT) bug had -- which every other load/save
    // function in this file already guards against with a full
    // g_audio_suspended (TriggerLoad(), TriggerSave(),
    // TriggerSaveGranularPreset(), TriggerLoadDexedPreset(),
    // TriggerSaveDexedPreset()). This was simply the one left out.
    // Yes, this does silence Dexed and the Looper layers too for the
    // ~1-2s a Grains load now takes (down from ~15s pre-fix) -- real
    // hardware showed that's not actually avoidable here, not an
    // unnecessary blunt hammer.
    g_audio_suspended = true;
    g_progress_disp   = disp_;
    // Loaded audio lands in the same owned capture buffer Direct Record/
    // From-Layer already use -- whichever source most recently filled it
    // is exactly what SetSource() should be pointing at.
    bool ok = PerformanceStore::LoadGranularPreset(
        slot, &preset, granular_capture_buf_l_, granular_capture_buf_r_,
        granular_capture_capacity_, &audio_len, &Ui::OnSaveLoadProgress);
    g_progress_disp   = nullptr;
    g_audio_suspended = false;

    if(ok)
    {
        granular_->ApplyPreset(preset);
        granular_->SetSource(granular_capture_buf_l_, granular_capture_buf_r_, audio_len);
        OnNewGranularCapture(audio_len);
        granular_loaded_preset_slot_ = slot;
        snprintf(granular_preset_status_, sizeof(granular_preset_status_), "Loaded GRAINS%d", slot);
    }
    else
    {
        snprintf(granular_preset_status_, sizeof(granular_preset_status_), "Fail:%s",
                  PerformanceStore::GetLastError());
    }
}

void Ui::RefreshDexedPresetSlots()
{
    dexed_import_folder_count_ = PerformanceStore::ListDexedImportFolders(
        dexed_import_folders_, PerformanceStore::kMaxDexedImportFolders);

    // Full unfiltered scan, then drop anything that belongs to a
    // recorded import folder -- what's left is the User group's own
    // list (see dexed_preset_user_slots_'s own doc comment). Needs the
    // import-folder refresh above to have already happened this call.
    static int raw_slots[kMaxDexedPresetSlots];
    int        raw_count = PerformanceStore::ListDexedPresets(raw_slots, kMaxDexedPresetSlots);
    int        filtered  = 0;
    for(int i = 0; i < raw_count && filtered < kMaxDexedPresetSlots; i++)
    {
        int  slot      = raw_slots[i];
        bool in_import = false;
        for(int f = 0; f < dexed_import_folder_count_; f++)
        {
            if(slot >= dexed_import_folders_[f].first_slot
               && slot <= dexed_import_folders_[f].last_slot)
            {
                in_import = true;
                break;
            }
        }
        if(!in_import)
            dexed_preset_user_slots_[filtered++] = slot;
    }
    dexed_preset_user_slot_count_ = filtered;

    int folder_count;
    switch(dexed_files_group_)
    {
        case DexedFilesGroup::Roms:
        case DexedFilesGroup::Dexed:
            folder_count = DexedSynth::GetFactoryCategoryCount(ResolveDexedRealCategory());
            break;
        case DexedFilesGroup::Imports: folder_count = dexed_import_folder_count_; break;
        case DexedFilesGroup::User:
        default: folder_count = dexed_preset_user_slot_count_; break;
    }
    if(dexed_preset_cursor_ >= folder_count)
        dexed_preset_cursor_ = folder_count > 0 ? folder_count - 1 : 0;
    dexed_preset_slots_dirty_ = false;
}

int Ui::ResolveDexedRealCategory() const
{
    if(dexed_files_group_ == DexedFilesGroup::Roms)
        return dexed_preset_folder_cursor_;
    if(dexed_files_group_ == DexedFilesGroup::Dexed)
        return dexed_preset_folder_cursor_ + DexedSynth::kNumRomCategories;
    return -1;
}

int Ui::ResolveDexedPresetSlot() const
{
    switch(dexed_files_group_)
    {
        case DexedFilesGroup::Roms:
        case DexedFilesGroup::Dexed:
            return DexedSynth::GetFactoryCategorySlot(ResolveDexedRealCategory(),
                                                        dexed_preset_cursor_);
        case DexedFilesGroup::Imports:
        {
            if(dexed_preset_folder_cursor_ < 0
               || dexed_preset_folder_cursor_ >= dexed_import_folder_count_)
                return -1;
            const PerformanceStore::DexedImportFolder& f
                = dexed_import_folders_[dexed_preset_folder_cursor_];
            int slot = f.first_slot + dexed_preset_cursor_;
            return slot <= f.last_slot ? slot : -1;
        }
        case DexedFilesGroup::User:
        default:
            if(dexed_preset_cursor_ < 0 || dexed_preset_cursor_ >= dexed_preset_user_slot_count_)
                return -1;
            return dexed_preset_user_slots_[dexed_preset_cursor_];
    }
}

void Ui::TriggerSaveDexedPreset(bool force_new)
{
    if(!dexed_)
        return;
    int slot = (!force_new && dexed_loaded_preset_slot_ > DexedSynth::GetNumFactoryPresets())
                   ? dexed_loaded_preset_slot_
                   : PerformanceStore::NextFreeDexedPresetSlot();
    if(slot < 0)
    {
        snprintf(dexed_preset_status_, sizeof(dexed_preset_status_), "Card full/missing");
        return;
    }
    // See the g_audio_suspended comment in TriggerSave() above. Dexed
    // presets are a single small struct (no chunked audio), so the write
    // is much shorter-lived than Performance/Grains, but it's still a
    // blocking SD write and the same race is possible in principle.
    g_audio_suspended = true;
    bool ok           = PerformanceStore::SaveDexedPreset(slot, dexed_->CapturePreset());
    g_audio_suspended = false;
    if(ok)
    {
        dexed_loaded_preset_slot_ = slot;
        snprintf(dexed_preset_status_, sizeof(dexed_preset_status_), "Saved %d", slot);
        dexed_preset_slots_dirty_ = true; // a new slot may now exist
    }
    else
    {
        snprintf(dexed_preset_status_, sizeof(dexed_preset_status_), "Fail:%s",
                  PerformanceStore::GetLastError());
    }
}

void Ui::TriggerNewDexedPreset()
{
    if(!dexed_)
        return;
    dexed_->ApplyPreset(DexedSynth::GetFactoryPreset(0));
    dexed_loaded_preset_slot_ = 1;
    snprintf(dexed_preset_status_, sizeof(dexed_preset_status_), "New (%s)",
              DexedSynth::GetFactoryPresetName(0));
}

void Ui::TriggerLoadDexedPreset()
{
    if(!dexed_)
        return;
    if(dexed_import_selected_)
    {
        TriggerDexedSyxImport();
        return;
    }
    if(!load_browsing_files_)
    {
        TriggerNewDexedPreset();
        return;
    }
    int slot = ResolveDexedPresetSlot();
    if(slot < 0)
        return;

    DexedSynth::DexedPresetData preset;
    bool ok = PerformanceStore::LoadDexedPreset(slot, &preset);
    if(ok)
    {
        dexed_->ApplyPreset(preset);
        dexed_loaded_preset_slot_ = slot;
        if(slot <= DexedSynth::GetNumFactoryPresets())
            snprintf(dexed_preset_status_, sizeof(dexed_preset_status_), "Loaded %s",
                      DexedSynth::GetFactoryPresetName(slot - 1));
        else
            snprintf(dexed_preset_status_, sizeof(dexed_preset_status_), "Loaded DEXED%d", slot);
    }
    else
    {
        snprintf(dexed_preset_status_, sizeof(dexed_preset_status_), "Fail:%s",
                  PerformanceStore::GetLastError());
    }
}

void Ui::RefreshDexedImportFiles()
{
    dexed_import_file_count_
        = PerformanceStore::ListImportSyxFiles(dexed_import_names_, kMaxImportFiles);
    if(dexed_import_cursor_ >= dexed_import_file_count_)
        dexed_import_cursor_ = dexed_import_file_count_ > 0 ? dexed_import_file_count_ - 1 : 0;
    dexed_import_files_dirty_ = false;
}

void Ui::TriggerDexedSyxImport()
{
    if(!dexed_ || dexed_import_file_count_ <= 0 || dexed_import_cursor_ < 0
       || dexed_import_cursor_ >= dexed_import_file_count_)
        return;

    // static -- 32 DexedPresetData (~6KB) is more than this function
    // wants to put on the stack for a UI action, same "large buffer ->
    // static" caution used throughout PerformanceStore's own SD I/O.
    static DexedSynth::DexedPresetData presets[PerformanceStore::kMaxSyxBulkVoices];
    int                                count = 0;
    // Suspend for the whole import: the .syx read plus up to 32 separate
    // SaveDexedPreset() writes below are each their own SD transaction,
    // same DMA-contention exposure as Performance Save/Load -- see
    // TriggerSave()'s own comment.
    g_audio_suspended = true;
    if(!PerformanceStore::ImportDexedSyx(dexed_import_names_[dexed_import_cursor_], presets, &count))
    {
        g_audio_suspended = false;
        snprintf(dexed_preset_status_, sizeof(dexed_preset_status_), "Fail:%s",
                  PerformanceStore::GetLastError());
        return;
    }

    // Each decoded voice becomes its own new user slot -- a Single Voice
    // Dump writes one, a 32-Voice Bulk Dump writes up to 32. Stops early
    // (keeping whatever was already written) if the card fills up
    // mid-import rather than losing the whole batch.
    //
    // Two real bugs fixed here, confirmed on hardware (a 32-voice import
    // looked completely frozen for several seconds): (1)
    // NextFreeDexedPresetSlot() used to be called fresh for every single
    // voice, each call re-scanning the WHOLE user slot range from its
    // own start -- O(n) work called n times is O(n^2) for the batch.
    // next_candidate instead resumes each search right where the last
    // one left off, one real forward sweep across the whole import.
    // (2) nothing drew anything to the display for the whole loop, so a
    // multi-second batch gave zero feedback that it was doing anything
    // at all -- this now shows the same "WORKING..." progress overlay
    // (Ui::OnSaveLoadProgress()) every other multi-step SD operation in
    // this project already uses.
    g_progress_disp = disp_;
    int first_slot = -1, last_slot = -1, next_candidate = -1;
    for(int i = 0; i < count; i++)
    {
        int slot = PerformanceStore::NextFreeDexedPresetSlot(next_candidate);
        if(slot < 0 || !PerformanceStore::SaveDexedPreset(slot, presets[i]))
            break;
        next_candidate = slot + 1;
        if(first_slot < 0)
            first_slot = slot;
        last_slot = slot;
        OnSaveLoadProgress((float)(i + 1) / (float)count);
    }
    g_progress_disp   = nullptr;
    g_audio_suspended = false;

    if(last_slot < 0)
    {
        snprintf(dexed_preset_status_, sizeof(dexed_preset_status_), "Card full/missing");
        return;
    }

    // Land on the last voice imported, live -- same "something real is
    // now loaded" feel as TriggerLoadDexedPreset()'s own preview.
    dexed_->ApplyPreset(presets[count - 1]);
    dexed_loaded_preset_slot_ = last_slot;
    dexed_preset_slots_dirty_ = true; // new user slot(s) now exist

    // Every import gets its own named folder (Files -> Imports group),
    // even a single-voice one -- named after the .syx file itself (the
    // SysEx data carries no overall "bank name" field of its own, only
    // each voice's individual 10-character name -- see
    // PerformanceStore::DexedImportFolder's own doc comment), extension
    // stripped. A failure here doesn't touch dexed_preset_status_ (the
    // "Imported->" message above already reported real success) --
    // it'd only mean this one import doesn't get its own folder and its
    // slots fall back to showing in the plain User list instead, not
    // that anything was lost.
    {
        const char* base = dexed_import_names_[dexed_import_cursor_];
        char        folder_name[24];
        snprintf(folder_name, sizeof(folder_name), "%s", base);
        size_t len = strlen(folder_name);
        if(len > 4)
        {
            // Case-insensitive ".syx" check, same manual-lowercase idiom
            // ListImportSyxFiles() already uses for the same extension.
            char ext[5];
            for(int i = 0; i < 4; i++)
                ext[i] = (char)tolower((unsigned char)folder_name[len - 4 + i]);
            ext[4] = '\0';
            if(strcmp(ext, ".syx") == 0)
                folder_name[len - 4] = '\0';
        }
        PerformanceStore::SaveDexedImportFolder(folder_name, first_slot, last_slot);
    }

    if(first_slot == last_slot)
        snprintf(dexed_preset_status_, sizeof(dexed_preset_status_), "Imported->%d", last_slot);
    else
        snprintf(dexed_preset_status_, sizeof(dexed_preset_status_), "Imported->%d-%d", first_slot,
                  last_slot);
}

void Ui::RefreshEnsemblePresetSlots()
{
    ensemble_preset_user_slot_count_ = PerformanceStore::ListEnsemblePresets(
        ensemble_preset_user_slots_, kMaxEnsemblePresetSlots);
    int total = EnsembleSynth::GetNumFactoryPresets() + ensemble_preset_user_slot_count_;
    if(ensemble_preset_cursor_ >= total)
        ensemble_preset_cursor_ = total > 0 ? total - 1 : 0;
    ensemble_preset_slots_dirty_ = false;
}

void Ui::TriggerSaveEnsemblePreset(bool force_new)
{
    if(!ensemble_)
        return;
    // Smart save (force_new=false): overwrite ensemble_loaded_preset_slot_
    // if it names a real USER slot; a factory slot or nothing loaded (0)
    // has no slot to overwrite, so fall back to a new one -- same
    // reasoning TriggerSaveDexedPreset() uses.
    int slot = (!force_new
                 && ensemble_loaded_preset_slot_ > EnsembleSynth::GetNumFactoryPresets())
                   ? ensemble_loaded_preset_slot_
                   : PerformanceStore::NextFreeEnsemblePresetSlot();
    if(slot < 0)
    {
        snprintf(ensemble_preset_status_, sizeof(ensemble_preset_status_), "Card full/missing");
        return;
    }
    // See the g_audio_suspended comment in TriggerSave() above -- same
    // short-lived single-struct write as TriggerSaveDexedPreset(), same
    // in-principle race.
    g_audio_suspended = true;
    bool ok           = PerformanceStore::SaveEnsemblePreset(slot, ensemble_->CapturePreset());
    g_audio_suspended = false;
    if(ok)
    {
        ensemble_loaded_preset_slot_ = slot;
        snprintf(ensemble_preset_status_, sizeof(ensemble_preset_status_), "Saved %d", slot);
        ensemble_preset_slots_dirty_ = true; // a new slot may now exist
    }
    else
    {
        snprintf(ensemble_preset_status_, sizeof(ensemble_preset_status_), "Fail:%s",
                  PerformanceStore::GetLastError());
    }
}

void Ui::TriggerNewEnsemblePreset()
{
    if(!ensemble_)
        return;
    ensemble_->ApplyPreset(EnsembleSynth::EnsemblePresetData{});
    ensemble_loaded_preset_slot_ = 0;
    snprintf(ensemble_preset_status_, sizeof(ensemble_preset_status_), "New");
}

void Ui::TriggerLoadEnsemblePreset()
{
    if(!ensemble_)
        return;
    // !load_browsing_files_ means the top-level chooser confirmed with
    // "New" highlighted (can_confirm_load only allows a confirm here when
    // that's the case -- see HandleButton2()).
    if(!load_browsing_files_)
    {
        TriggerNewEnsemblePreset();
        return;
    }
    int factory_count = EnsembleSynth::GetNumFactoryPresets();
    int total          = factory_count + ensemble_preset_user_slot_count_;
    if(ensemble_preset_cursor_ < 0 || ensemble_preset_cursor_ >= total)
        return;
    // Resolve the combined [factory][user] list directly into a real
    // slot number -- cursor < factory_count means a factory preset
    // (slots are 1-based, matching PerformanceStore::LoadEnsemblePreset()'s
    // own factory-range convention), otherwise a real SD user slot.
    int slot = ensemble_preset_cursor_ < factory_count
                   ? ensemble_preset_cursor_ + 1
                   : ensemble_preset_user_slots_[ensemble_preset_cursor_ - factory_count];

    EnsembleSynth::EnsemblePresetData preset;
    bool ok = PerformanceStore::LoadEnsemblePreset(slot, &preset);
    if(ok)
    {
        ensemble_->ApplyPreset(preset);
        ensemble_loaded_preset_slot_ = slot;
        if(slot <= factory_count)
            snprintf(ensemble_preset_status_, sizeof(ensemble_preset_status_), "Loaded %s",
                      EnsembleSynth::GetFactoryPresetName(slot - 1));
        else
            snprintf(ensemble_preset_status_, sizeof(ensemble_preset_status_), "Loaded ENSM%d", slot);
    }
    else
    {
        snprintf(ensemble_preset_status_, sizeof(ensemble_preset_status_), "Fail:%s",
                  PerformanceStore::GetLastError());
    }
}

void Ui::TriggerSaveDefaults()
{
    bool ok = PerformanceStore::SavePrefs(*tempo_, master_volume01_, bypass_,
                                            master_filter_mode_, master_filter_cutoff01_,
                                            master_filter_res01_, reverb_size01_,
                                            bypass_reverb_send01_, reverb_predelay01_,
                                            reverb_prefilter01_, reverb_input_diffusion1_01_,
                                            reverb_input_diffusion2_01_,
                                            reverb_decay_diffusion01_, reverb_damping01_,
                                            dexed_midi_channel_, granular_midi_channel_,
                                            ensemble_midi_channel_, sample_synth_midi_channel_);
    if(ok)
        snprintf(tempo_status_, sizeof(tempo_status_), "Saved as default");
    else
        snprintf(tempo_status_, sizeof(tempo_status_), "Fail:%s", PerformanceStore::GetLastError());
}

void Ui::TriggerUndoOverdub()
{
    if(!undo_owner_layer_ || *undo_owner_layer_ != cursor_layer_)
    {
        snprintf(undo_status_, sizeof(undo_status_), "Nothing to undo");
        undo_status_set_ms_ = System::GetNow();
        return;
    }
    Cur().RestoreFromBackup(undo_backup_l_, undo_backup_r_, *undo_backup_len_);
    // Single-level undo, same "restores the one most recent action" shape
    // as every pedal looper's own Undo -- invalidate ownership so a
    // second tap can't re-apply the same restore (or, worse, restore
    // from a backup a DIFFERENT layer has since overwritten).
    *undo_owner_layer_ = -1;
    undo_status_[0]    = '\0'; // the restored waveform is its own feedback
}

void Ui::TriggerExport()
{
    // ExportWav() drives every layer's real Process() an extra time from
    // the main loop to render the mix -- must not race the live ISR
    // doing the same on the same objects, same reasoning as TriggerLoad().
    g_audio_suspended       = true;
    g_progress_disp         = disp_;
    export_op_in_progress_  = true;
    bool ok = PerformanceStore::ExportWav(*tempo_, layers_, num_layers_, master_filter_mode_,
                                            master_filter_cutoff01_, master_filter_res01_,
                                            reverb_size01_, reverb_predelay01_,
                                            reverb_prefilter01_, reverb_input_diffusion1_01_,
                                            reverb_input_diffusion2_01_,
                                            reverb_decay_diffusion01_, reverb_damping01_,
                                            delay_time01_, delay_feedback01_,
                                            /*for_microdexed=*/false, project_speed_,
                                            &Ui::OnSaveLoadProgress);
    export_op_in_progress_  = false;
    g_progress_disp         = nullptr;
    g_audio_suspended       = false;

    if(ok)
        snprintf(export_status_, sizeof(export_status_), "Exported WAV");
    else
        snprintf(export_status_, sizeof(export_status_), "Fail:%s",
                  PerformanceStore::GetLastError());
}

void Ui::TriggerExportMicroDexed()
{
    // Same reasoning as TriggerExport() above -- must not race the live
    // audio ISR while this drives an extra offline render pass.
    g_audio_suspended       = true;
    g_progress_disp         = disp_;
    export_op_in_progress_  = true;
    bool ok = PerformanceStore::ExportWav(*tempo_, layers_, num_layers_, master_filter_mode_,
                                            master_filter_cutoff01_, master_filter_res01_,
                                            reverb_size01_, reverb_predelay01_,
                                            reverb_prefilter01_, reverb_input_diffusion1_01_,
                                            reverb_input_diffusion2_01_,
                                            reverb_decay_diffusion01_, reverb_damping01_,
                                            delay_time01_, delay_feedback01_,
                                            /*for_microdexed=*/true, project_speed_,
                                            &Ui::OnSaveLoadProgress);
    export_op_in_progress_  = false;
    g_progress_disp         = nullptr;
    g_audio_suspended       = false;

    if(ok)
        snprintf(export_status_, sizeof(export_status_), "Exported 44.1k");
    else
        snprintf(export_status_, sizeof(export_status_), "Fail:%s",
                  PerformanceStore::GetLastError());
}

void Ui::OnSaveLoadProgress(float progress01)
{
    if(!g_progress_disp)
        return;
    // Throttled: a full 4-layer stereo loop streams in ~4096-sample
    // chunks, and redrawing the (blocking I2C) OLED on every single
    // chunk would noticeably slow the transfer down for no visible
    // benefit -- a few redraws a second is plenty for a progress bar.
    static uint32_t call_count = 0;
    call_count++;
    // Always draw the first and last frame of any sequence -- only
    // throttle the ones in between. Without the >0.001f half of this,
    // a short batch (e.g. a 2-3 file Import sweep) could hit every
    // multiple of 8 only after the whole thing's already done, so the
    // very first frame the user ever saw was 100% -- the overlay
    // looked like it "didn't come up straight away" when really it
    // just never got a turn to draw at all until the end.
    if(call_count % 8 != 0 && progress01 > 0.001f && progress01 < 0.999f)
        return;

    OneBitGraphicsDisplay* d = g_progress_disp;
    d->Fill(false);
    d->SetCursor(0, 14);
    // Font_7x10, not the footer's usual Font_6x8 -- bigger, more visible
    // for a modal progress overlay. Uppercase like everything else drawn
    // with Font_6x8 (see Ui::WriteUpper()) -- this one's Font_7x10 and a
    // literal, not routed through that helper, so it's just spelled
    // uppercase directly here instead.
    d->WriteString("WORKING...", Font_7x10, true);
    int w = (int)(Clampf(progress01, 0.f, 1.f) * (d->Width() - 2));
    d->DrawRect(0, 32, d->Width() - 1, 40, true, false);
    if(w > 0)
        d->DrawRect(1, 33, w, 39, true, true);
    d->Update();
}

void Ui::OnSampleSynthImportFileProgress(float sub01)
{
    OnSaveLoadProgress(g_progress_file_base01 + sub01 * g_progress_file_span01);
}
