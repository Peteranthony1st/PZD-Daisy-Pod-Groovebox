#include "ui.h"
#include "itcm.h"
#include <utility>
#include "profiler.h"
#include "performance_store.h"
#include "audio_engine.h"
#include "shared_delay.h"
#include "granular_engine.h"
#include "dexed_synth.h"
#include "sample_synth.h"
#include "msfa/fm_core.h"
#include "ui_graphics.h"
#include <cstdio>
#include <cstring>
#include <cctype>
#include <math.h>

// msfa's own keyboard level scaling (dx7note.cpp) -- drawn by
// DrawLevelScalingGraph() so the graph matches what the engine plays.
int ScaleLevel(int midinote, int break_pt, int left_depth, int right_depth, int left_curve,
               int right_curve);

using namespace daisy;

namespace
{
inline float Clampf(float v, float lo, float hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

// Every engine's Gain01 is 0 to +12 dB (powf(4, v01)), shown in dB --
// tenths in integer maths, since this build's printf has no float support.
void FormatGainDb(float gain01, char* out, size_t out_size)
{
    int tenths = (int)(gain01 * 120.4f + 0.5f);
    snprintf(out, out_size, "+%d.%ddB", tenths / 10, tenths % 10);
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

// Grains readouts in the GR-1's own units.
void FormatGranularSize(const GranularEngine* g, char* out, size_t n)
{
    snprintf(out, n, "%dms", (int)(g->GetSizeMs() + 0.5f));
}
void FormatGranularDensity(const GranularEngine* g, char* out, size_t n)
{
    float hz = g->GetDensityHz();
    if(hz < 10.f)
        snprintf(out, n, "%d.%dHz", (int)hz, (int)(hz * 10.f) % 10);
    else
        snprintf(out, n, "%dHz", (int)(hz + 0.5f));
}
void FormatGranularScan(const GranularEngine* g, char* out, size_t n)
{
    float x = g->GetScanSpeed();
    if(x == 0.f)
    {
        snprintf(out, n, "Hold");
        return;
    }
    int c = (int)(fabsf(x) * 100.f + 0.5f);
    snprintf(out, n, "%sx%d.%02d", x < 0.f ? "-" : "", c / 100, c % 100);
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
    dexed_engines_[0]      = dexed;
    dexed_engines_[1]      = dexed_b;
    dexed_scope_buf_        = dexed_scope_buf;
    dexed_scope_capacity_   = dexed_scope_capacity;
    ensemble_               = ensemble;
    ensemble_engines_[0]    = ensemble;
    ensemble_engines_[1]    = organ;
    ensemble_scope_buf_      = ensemble_scope_buf;
    ensemble_scope_capacity_ = ensemble_scope_capacity;
    sample_synth_                = sample_synth;
    sample_synth_engines_[0]     = sample_synth;
    sample_synth_engines_[1]     = sample_drum;
    sample_synth_scope_buf_      = sample_synth_scope_buf;
    sample_synth_scope_capacity_ = sample_synth_scope_capacity;
    bypass_scope_buf_            = bypass_scope_buf;
    bypass_scope_capacity_       = bypass_scope_capacity;
    // main.cpp's block holds 4 rows: Voice L, Voice R, Drum L, Drum R.
    for(int f = 0; f < 2; f++)
        for(int c = 0; c < 2; c++)
            sample_synth_arenas_[f][c]
                = sample_synth_arena + (size_t)(f * 2 + c) * sample_synth_arena_capacity;
    sample_synth_arena_l_        = sample_synth_arenas_[0][0];
    sample_synth_arena_r_        = sample_synth_arenas_[0][1];
    sample_synth_arena_capacity_ = sample_synth_arena_capacity;
    // Two rows of int16 = one row of float, so rows 0-1 and 2-3 are
    // each a float buffer of the same sample count.
    sample_synth_scratch_l_ = reinterpret_cast<float*>(sample_synth_arenas_[0][0]);
    sample_synth_scratch_r_ = reinterpret_cast<float*>(sample_synth_arenas_[1][0]);
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
    int        bars, dexed_ch, granular_ch, ensemble_ch, sample_synth_ch; // read, not applied
    bool       byp, metro_on;
    FilterMode mode;
    // Global:Looper's power-up default, its own file -- read first, since
    // a card with no PREFS.DAT returns early below. Nothing is recorded
    // yet at boot, so starting disabled needs none of B1's resets.
    if(PerformanceStore::LoadLooperDefault(&looper_default_on_))
        looper_enabled_ = looper_default_on_;
    PerformanceStore::LoadPedalReversed(&pedal_reversed_);
    {
        uint32_t mins;
        if(PerformanceStore::LoadScreenSaver(&mins))
            for(int o = 0; o < kSaverOptions; o++)
                if((uint32_t)SaverMinutes(o) == mins)
                    saver_option_ = saver_default_ = o;
    }
    saver_touch_ms_ = System::GetNow();
    // Every engine's MIDI channel, as saved by Global:MIDI's Hold=Default
    // (Ch1, SMPDR Ch10 until then). Not part of Global:Tempo's defaults.
    {
        int channels[PerformanceStore::kMidiDefaultEngines];
        for(int e = 0; e < PerformanceStore::kMidiDefaultEngines; e++)
            channels[e] = *MidiDefaultChannel(e);
        if(PerformanceStore::LoadMidiDefaults(channels))
            for(int e = 0; e < PerformanceStore::kMidiDefaultEngines; e++)
                *MidiDefaultChannel(e) = channels[e];
    }

    // Starts as the current values -- only overwritten by a v16+ file.
    PerformanceStore::DelayState prefs_delay = GetDelayState();
    if(!PerformanceStore::LoadPrefs(&bpm, &bars, &master_vol01, &byp, &mode, &cutoff01,
                                      &res01, &reverb_sz01, &byp_send01, &metro_on,
                                      &metro_vol01, &rev_predelay01, &rev_prefilter01,
                                      &rev_diff1_01, &rev_diff2_01, &rev_decdiff01,
                                      &rev_damping01, &dexed_ch, &granular_ch, &ensemble_ch,
                                      &sample_synth_ch, &prefs_delay))
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
    ApplyDelayState(prefs_delay);

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

namespace
{
uint32_t g_splash_shown_ms = 0; // when DrawSplash() put it up
}

void Ui::DrawSplash(daisy::OneBitGraphicsDisplay* display)
{
    display->Fill(false);
    DrawBitmap(display, 0, 0, kSplashW, kSplashH, kSplashBitmap);
    display->Update();
    g_splash_shown_ms = System::GetNow();
}

void Ui::Update(const UiControlEvents& events)
{
    UpdateSeqPrint();
    UpdateLiveExport();
    UpdateScenes();
    UpdateSong();
    // The splash is already on screen (see DrawSplash()); audio and MIDI
    // run as normal underneath, only the UI waits. A press skips it and
    // is swallowed, so it doesn't also act on Home.
    if(splash_active_)
    {
        bool pressed = events.encoder_click_fell || events.btn1_released || events.btn2_released;
        if(!pressed && System::GetNow() - g_splash_shown_ms < kSplashMs)
        {
            UpdateLeds();
            return;
        }
        splash_active_ = false;
        draw_counter_  = 20; // redraw straight away below
        if(pressed)
        {
            Draw();
            draw_counter_ = 0;
            return;
        }
    }

    // Triple-click panic. A click that ends a long press isn't a click.
    UiControlEvents ev = events;
    if(ev.encoder_click_fell && !encoder_long_fired_)
    {
        uint32_t now = System::GetNow();
        if(panic_clicks_ == 0 || now - panic_first_click_ms_ > kPanicWindowMs)
        {
            panic_clicks_         = 0;
            panic_first_click_ms_ = now;
            panic_return_screen_  = screen_;
        }
        if(++panic_clicks_ >= 3)
        {
            panic_clicks_      = 0;
            panic_requested_   = true;
            panic_shown_ms_    = now;
            screen_            = panic_return_screen_;
            ev.encoder_click_fell = false; // the third click only panics
        }
    }

    if(UpdateSaver(ev))
    {
        UpdateLeds();
        return; // asleep or just woken: the controls do nothing, no redraw while off
    }
    HandleEncoder(ev);
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

// --- Screen saver ----------------------------------------------------------

int Ui::SaverMinutes(int option)
{
    static const int kMinutes[kSaverOptions] = {0, 1, 2, 5, 10, 30};
    return option >= 0 && option < kSaverOptions ? kMinutes[option] : 0;
}

void Ui::SetSaver(Saver s)
{
    if(s == saver_ || !display_cmd_)
    {
        saver_ = s;
        return;
    }
    if(s == Saver::Off)
        display_cmd_(0xAE); // panel off
    else
    {
        if(saver_ == Saver::Off)
            display_cmd_(0xAF); // panel on
        display_cmd_(0x81);    // contrast
        display_cmd_(s == Saver::Dim ? 0x01 : 0x8F); // 0x8F: the driver's own
    }
    saver_ = s;
}

bool Ui::UpdateSaver(const UiControlEvents& ev)
{
    const uint32_t now = System::GetNow();
    const float    k1  = pod_->GetKnobValue(DaisyPod::KNOB_1);
    const float    k2  = pod_->GetKnobValue(DaisyPod::KNOB_2);
    const bool     held = pod_->button1.Pressed() || pod_->button2.Pressed()
                      || pod_->encoder.Pressed();
    // A knob counts once it's clearly moved, not for its usual jitter.
    const bool knobs = saver_k1_ < 0.f || fabsf(k1 - saver_k1_) > 0.03f
                       || fabsf(k2 - saver_k2_) > 0.03f;
    const bool touched = ev.encoder_delta != 0 || ev.encoder_click_fell || ev.btn1_released
                         || ev.btn2_released || held || knobs;
    if(touched)
    {
        saver_touch_ms_ = now;
        saver_k1_       = k1;
        saver_k2_       = k2;
    }
    if(saver_swallow_)
    {
        // The waking touch: over once nothing's held and nothing's arriving.
        if(!held && !ev.encoder_click_fell && !ev.btn1_released && !ev.btn2_released)
        {
            saver_swallow_ = false;
            ResyncKnobs(); // the knobs pick their values up again
            button2_long_fired_ = false;
        }
        return true;
    }
    if(saver_ != Saver::Awake && touched)
    {
        SetSaver(Saver::Awake);
        saver_swallow_ = true;
        draw_counter_  = 20; // redraw straight away
        return true;
    }
    // Dim after the timeout, off a minute after that.
    const int      mins = SaverMinutes(saver_option_);
    const uint32_t idle = now - saver_touch_ms_;
    if(mins > 0 && saver_ == Saver::Awake && idle > (uint32_t)mins * 60000u)
        SetSaver(Saver::Dim);
    if(mins > 0 && saver_ == Saver::Dim && idle > (uint32_t)mins * 60000u + 60000u)
        SetSaver(Saver::Off);
    // Dimmed, it still draws and the controls would work -- but any touch
    // wakes it first (above); off, nothing draws.
    return saver_ == Saver::Off;
}

void Ui::DrawDisplayPage()
{
    disp_->SetCursor(0, 0);
    WriteUpper("Global:Display");
    DrawBeatIndicator(disp_->Width() - 41, 0, 3);
    disp_->DrawLine(0, 9, disp_->Width() - 1, 9, true);

    TomThumbDrawText(disp_, 1, 16, "SCREEN SAVER", true);
    char def[24];
    const int dmins = SaverMinutes(saver_default_);
    if(saver_status_[0] != '\0')
        snprintf(def, sizeof(def), "%s", saver_status_);
    else if(dmins > 0)
        snprintf(def, sizeof(def), "DEFAULT:%d MIN", dmins);
    else
        snprintf(def, sizeof(def), "DEFAULT:OFF");
    TomThumbDrawText(disp_, disp_->Width() - 1 - TomThumbAdvanceWidth(def), 16, def, true);
    char line[24];
    const int mins = SaverMinutes(saver_option_);
    if(mins > 0)
        snprintf(line, sizeof(line), "DIM AFTER %d MIN", mins);
    else
        snprintf(line, sizeof(line), "OFF");
    disp_->SetCursor((disp_->Width() - (int)strlen(line) * 6) / 2, 24);
    disp_->WriteString(line, Font_6x8, true);
    const char* hint = mins > 0 ? "THEN OFF A MINUTE LATER" : "THE SCREEN STAYS ON";
    TomThumbDrawText(disp_, (disp_->Width() - TomThumbAdvanceWidth(hint)) / 2, 38, hint, true);
    const char* hint2 = "ANY TOUCH WAKES IT";
    TomThumbDrawText(disp_, (disp_->Width() - TomThumbAdvanceWidth(hint2)) / 2, 44, hint2, true);

    char tv[8];
    if(mins > 0)
        snprintf(tv, sizeof(tv), "%d min", mins);
    else
        snprintf(tv, sizeof(tv), "Off");
    DrawControlRow(kFooterRow1Y, false, kFooterDividerY, "Time", tv, "", "");
    DrawControlRow(kFooterRow2Y, true, kFooterInterRowDividerY, "Sleep now", "", "", "Default");
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
        else if(screen_ == Screen::SeqTrack)
        {
            int n     = kSeqPageGrid + SeqGridBars(HomeSeqTrack());
            seq_page_ = ((seq_page_ + inc) % n + n) % n;
            grid_edit_ = false;
        }
        else if(screen_ == Screen::Home && HomeScenes() && scene_edit_ >= 0)
            ; // editing a scene: K1 moves around it, the page stays
        else if(screen_ == Screen::Home)
        {
            // Past layer 4 into the sequencer tracks, four to a page.
            home_slot_ = ((home_slot_ + inc) % kHomeSlots + kHomeSlots) % kHomeSlots;
            if(home_slot_ < num_layers_)
                cursor_layer_ = home_slot_;
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
            if(new_page == GlobalPage::Snap && global_page_ != GlobalPage::Snap)
            {
                snap_slots_dirty_ = true;
                snap_status_[0]   = '\0';
            }
            if(new_page == GlobalPage::Export && global_page_ != GlobalPage::Export)
                export_status_[0] = '\0'; // clear any stale result on entry
            if(new_page == GlobalPage::Midi && global_page_ != GlobalPage::Midi)
                midi_status_[0] = '\0';
            if(new_page == GlobalPage::SdMgmt && global_page_ != GlobalPage::SdMgmt)
            {
                sd_mgmt_in_folder_ = false; // always start back at folder-select on entry
                // Its Ensemble box is the Ensemble's own folder.
                SelectEnsembleEngine(0);
            }
            global_page_    = new_page;
            save_load_mode_ = SaveLoadMode::Idle; // leaving/entering any page resets this
        }
        else if(screen_ == Screen::Granular)
        {
            int n = (int)GranularParamPage::kCount;
            int p = (((int)granular_param_page_ + inc) % n + n) % n;
            GranularParamPage new_granular_page = (GranularParamPage)p;
            if(new_granular_page != granular_param_page_)
            {
                // Re-scan the card on entry to either list page.
                if(new_granular_page == GranularParamPage::Preset)
                {
                    granular_lib_dirty_       = true;
                    granular_lib_items_dirty_ = true;
                }
                else if(new_granular_page == GranularParamPage::User)
                    granular_preset_slots_dirty_ = true;
                granular_trim_status_[0] = '\0';
            }
            granular_param_page_ = new_granular_page;
            save_load_mode_      = SaveLoadMode::Idle; // same reset as Global:File above
        }
        else if(screen_ == Screen::Dexed)
        {
            int n = (int)DexedParamPage::kCount;
            int p = (((int)dexed_param_page_ + inc) % n + n) % n;
            DexedParamPage new_dexed_page = (DexedParamPage)p;
            if(new_dexed_page != dexed_param_page_)
            {
                // Re-scan the card on entry to any of the list pages.
                if(new_dexed_page == DexedParamPage::Preset
                   || new_dexed_page == DexedParamPage::User)
                    dexed_preset_slots_dirty_ = true;
                if(new_dexed_page == DexedParamPage::Import)
                {
                    dexed_import_files_dirty_ = true;
                    dexed_preset_status_[0]   = '\0';
                }
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
            // Perc/Click are Organ-only, the LFO pages Ensemble-only: step
            // past the other engine's.
            const int  step  = inc >= 0 ? 1 : -1;
            const bool organ = ensemble_ && ensemble_->IsOrgan();
            while((!organ
                   && (p == (int)EnsembleParamPage::Perc || p == (int)EnsembleParamPage::Click))
                  || (organ
                      && (p == (int)EnsembleParamPage::Lfo
                          || p == (int)EnsembleParamPage::LfoDepth)))
                p = ((p + step) % n + n) % n;
            EnsembleParamPage new_ensemble_page = (EnsembleParamPage)p;
            if((new_ensemble_page == EnsembleParamPage::Preset
                || new_ensemble_page == EnsembleParamPage::User)
               && ensemble_param_page_ != new_ensemble_page)
                ensemble_preset_slots_dirty_ = true; // re-scan the card on entry
            ensemble_param_page_ = new_ensemble_page;
            save_load_mode_      = SaveLoadMode::Idle; // same reset as Global:File above
        }
        else if(screen_ == Screen::Supersaw)
            SupersawRotatePage(inc);
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
            else if(new_page == SampleSynthParamPage::Import
                    && sample_synth_param_page_ != SampleSynthParamPage::Import)
            {
                sample_synth_import_dirty_          = true; // re-scan what's waiting
                sample_synth_import_drum_open_      = false;
                sample_synth_pack_import_status_[0] = '\0';
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
        else if(screen_ == Screen::Arp)
            arp_page_ = ((arp_page_ + inc) % kArpPages + kArpPages) % kArpPages;
        else if(screen_ == Screen::Mixer)
        {
            int n            = MixerSetCount() + 1; // + the Scope stop
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
            global_page_ = GlobalPage::MixTracks;
            // This hand-wired SD socket has no card-detect pin, so the
            // firmware otherwise never notices a card was swapped out
            // (e.g. pulled to move a WAV export to another device) and
            // put back -- re-mount here so Global:File/Export reflect
            // whatever card is actually in the slot right now, rather
            // than a stale mount from boot. Not while a live export is
            // writing -- re-mounting would pull its open file away.
            if(!live_open_)
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
            if(HomeScenes())
            {
                if(scene_edit_ >= 0)
                    scene_edit_ = -1;
                else
                {
                    // An empty scene starts as what's playing.
                    if(!scenes_[scene_cursor_].used)
                        SaveScene(scene_cursor_);
                    scene_edit_ = scene_cursor_;
                }
            }
            else if(HomeSong())
                song_edit_ = !song_edit_;
            else if(HomeSeqTrack() >= 0)
            {
                screen_   = Screen::SeqTrack;
                seq_page_ = kSeqPageTrack;
                if(seq_)
                    seq_patt_cursor_ = seq_->GetPattern(HomeSeqTrack());
                // The grid starts on the track's first row.
                grid_col_  = 0;
                grid_note_ = 36;
                grid_top_  = 36;
            }
            else
            {
                screen_     = Screen::Layer;
                layer_page_ = LayerPage::Status;
            }
        }
        else if(!encoder_long_fired_ && screen_ == Screen::SeqTrack && seq_page_ == kSeqPageArp
                && arp_ && HomeSeqTrack() >= 0)
        {
            screen_      = Screen::Arp; // the pattern K1 picked there
            arp_page_    = 0;
            arp_track_   = HomeSeqTrack();
            arp_pattern_ = seq_patt_cursor_;
        }
        else if(!encoder_long_fired_ && screen_ == Screen::SeqTrack)
            screen_ = Screen::Home;
        else if(!encoder_long_fired_ && screen_ == Screen::Arp && arp_track_ >= 0)
        {
            screen_   = Screen::SeqTrack; // back to the track's Arp page
            seq_page_ = kSeqPageArp;
        }
        else if(!encoder_long_fired_ && screen_ == Screen::Layer)
        {
            // Click backs out one level, the same on every screen: Layer
            // -> Home, an engine or the Mixer -> its Global page.
            screen_ = Screen::Home;
        }
        else if(!encoder_long_fired_ && screen_ == Screen::Global
                && global_page_ == GlobalPage::Granular)
        {
            // Global:Granular is an entry point into Screen::Granular,
            // same convention Screen::Mixer's own Global page uses.
            screen_ = Screen::Granular;
        }
        else if(!encoder_long_fired_ && screen_ == Screen::Global
                && global_page_ == GlobalPage::Arp && arp_)
        {
            screen_    = Screen::Arp; // the engine K1 picked there
            arp_page_  = 0;
            arp_track_ = -1;
        }
        else if(!encoder_long_fired_ && screen_ == Screen::Global
                && (global_page_ == GlobalPage::MixTracks || global_page_ == GlobalPage::MixSynths))
        {
            // Global:Tracks/Global:Synths are entry points into
            // Screen::Mixer, which then steps through that page's set.
            const int set = global_page_ == GlobalPage::MixSynths ? 1 : 0;
            if(set != mixer_set_)
            {
                mixer_set_      = set;
                mixer_position_ = 0;
            }
            screen_ = Screen::Mixer;
        }
        else if(!encoder_long_fired_ && screen_ == Screen::Global
                && (global_page_ == GlobalPage::Dexed || global_page_ == GlobalPage::DexedB))
        {
            // Global:DXD-A/DXD-B are entry points into Screen::Dexed, same
            // convention Global:Granular's own click uses -- which one
            // just picks the engine its pages edit.
            SelectDexedEngine(global_page_ == GlobalPage::DexedB ? 1 : 0);
            screen_ = Screen::Dexed;
        }
        else if(!encoder_long_fired_ && screen_ == Screen::Global
                && (global_page_ == GlobalPage::Ensemble || global_page_ == GlobalPage::Organ))
        {
            // Global:Ensemble/Global:Organ are entry points into
            // Screen::Ensemble, same convention Global:Granular/
            // Global:Dexed's own click uses -- which one picks the engine
            // its pages edit, starting on Registration.
            int engine = global_page_ == GlobalPage::Organ ? 1 : 0;
            if(engine != ensemble_active_)
            {
                SelectEnsembleEngine(engine);
                ensemble_param_page_    = EnsembleParamPage::Registration;
                ensemble_drawbar_index_ = 0;
                ensemble_preset_cursor_ = 0;
                ensemble_user_cursor_   = 0;
            }
            screen_ = Screen::Ensemble;
        }
        else if(!encoder_long_fired_ && screen_ == Screen::Global
                && global_page_ == GlobalPage::Supersaw)
        {
            // Global:Supersaw is the entry point into Screen::Supersaw,
            // as Global:Ensemble's is into Screen::Ensemble.
            screen_ = Screen::Supersaw;
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
            // Entering doesn't switch it on -- Button1 does, same as
            // every other engine.
            screen_ = Screen::SampleSynth;
        }
        else if(!encoder_long_fired_ && screen_ == Screen::Global)
        {
            // Every other Global page (nothing to enter): click backs out
            // to Home, same as a hold. Pause lives on Home's Button1;
            // Global:Looper switches the whole looper off.
            screen_ = Screen::Home;
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
                    || screen_ == Screen::Ensemble || screen_ == Screen::SampleSynth
                    || screen_ == Screen::Mixer || screen_ == Screen::Arp
                    || screen_ == Screen::Supersaw))
        {
            // Back out to the Global page this screen was entered from --
            // global_page_ is left untouched while inside, so this lands on
            // Global:Granular/Dexed/Ensemble/SMPVO/SMPDR/Mixer. (Dexed's
            // Advanced page is handled above: its click enters the
            // operator editor.) Hold still goes straight to Home.
            screen_ = Screen::Global;
        }
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

void Ui::HandleArpVariationButton(Switch& b, bool released, int dir, bool& repeating)
{
    // Arp Feel's Var -/+: a tap steps once; held, it repeats, faster and
    // faster (from 150ms apart down to 25ms), so 99 back to 0 is a few
    // seconds' hold.
    Arpeggiator::Settings& s    = ArpEdit();
    auto                   step = [&] {
        const int v = s.variation + dir;
        if(v >= 0 && v < Arpeggiator::kNumVariations)
            s.variation = (uint8_t)v;
    };
    if(b.Pressed() && b.TimeHeldMs() > 400.f)
    {
        const float t = b.TimeHeldMs();
        if(!repeating)
        {
            repeating          = true;
            arp_var_next_ms_   = t;
            arp_var_repeats_   = 0;
        }
        while(t >= arp_var_next_ms_)
        {
            step();
            const int every = 150 - 10 * arp_var_repeats_;
            arp_var_next_ms_ += (float)(every > 25 ? every : 25);
            arp_var_repeats_++;
        }
    }
    if(released)
    {
        if(!repeating)
            step();
        repeating = false;
    }
}

void Ui::HandleButton1(const UiControlEvents& events)
{
    Switch& b = pod_->button1;
    if(screen_ == Screen::Arp && arp_page_ == 2 && arp_)
    {
        HandleArpVariationButton(b, events.btn1_released, -1, button1_long_fired_);
        return;
    }
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
    if(screen_ == Screen::Arp && arp_page_ == 2 && arp_)
    {
        HandleArpVariationButton(b, events.btn2_released, +1, button2_long_fired_);
        return;
    }
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
    else if(screen_ == Screen::Global && global_page_ == GlobalPage::SdMgmt)
    {
        // Shared browse convention: B2 taps open (a box, then any folder
        // level inside it); in an item list B2 is hold-to-delete, with a
        // longer hold than every other confirm here (see
        // kSdMgmtDeleteHoldMs's own comment) -- delete has no undo.
        bool in_items = sd_mgmt_in_folder_ && !SdMgmtAtFolderList();
        if(in_items && b.Pressed() && b.TimeHeldMs() > kSdMgmtDeleteHoldMs
           && !button2_long_fired_)
        {
            button2_long_fired_ = true;
            TriggerSdMgmtDelete();
        }
        if(events.btn2_released)
        {
            if(!sd_mgmt_in_folder_)
                SdMgmtOpenBox();
            else if(!in_items && SdMgmtGlobalBox())
            {
                sd_mgmt_global_kind_ = sd_mgmt_list_cursor_ > 0 ? 1 : 0; // 0 Perf, 1 Snapshots
                sd_mgmt_cursor_      = 0;
                sd_mgmt_status_[0]   = '\0';
            }
            else if(!in_items && SdMgmtEnsembleBox())
            {
                // Ensemble or Organ: point the preset functions (and the
                // loaded-slot tracking Delete updates) at that engine. The
                // Supersaw (2) has its own.
                sd_mgmt_ens_kind_ = sd_mgmt_list_cursor_ > 2 ? 2 : sd_mgmt_list_cursor_;
                if(sd_mgmt_ens_kind_ < 2)
                {
                    SelectEnsembleEngine(sd_mgmt_ens_kind_);
                    ensemble_preset_slots_dirty_ = true;
                }
                else
                    ssw_slots_dirty_ = true;
                sd_mgmt_cursor_              = 0;
                sd_mgmt_status_[0]           = '\0';
            }
            else if(!in_items && SdMgmtDrumUserBox())
            {
                sd_mgmt_drum_kind_ = sd_mgmt_list_cursor_ > 0 ? 1 : 0; // 0 Kits, 1 Breaks
                sd_mgmt_cursor_    = 0;
                sd_mgmt_status_[0] = '\0';
            }
            else if(!in_items)
            {
                int n = SdMgmtBankLevelCount();
                if(sd_mgmt_list_cursor_ < n)
                {
                    if(sd_mgmt_folder_ == SdMgmtFolder::SampleSynthCaptures
                       && sd_mgmt_capt_kind_ < 0)
                    {
                        sd_mgmt_capt_kind_   = sd_mgmt_list_cursor_; // 0 Sampler, 1 Grains
                        sd_mgmt_banks_dirty_ = true;
                    }
                    else if(SdMgmtSmpChoosing())
                    {
                        if(sd_mgmt_smp_kind_ < 0)
                            sd_mgmt_smp_kind_ = sd_mgmt_list_cursor_; // 0 Voice, 1 Drum
                        else
                            sd_mgmt_smp_sub_kind_ = sd_mgmt_list_cursor_; // Sounds/Multi, Kits/Breaks
                        sd_mgmt_banks_dirty_ = true;
                    }
                    else
                    {
                        sd_mgmt_bank_        = sd_mgmt_list_cursor_;
                        sd_mgmt_cursor_      = 0;
                        sd_mgmt_items_dirty_ = true;
                    }
                    sd_mgmt_status_[0] = '\0';
                }
            }
            else if(!button2_long_fired_ && events.btn2_held_ms > kSdMgmtDeleteHoldMs)
                TriggerSdMgmtDelete();
            button2_long_fired_ = false;
        }
    }
    else if(screen_ == Screen::Global && global_page_ == GlobalPage::Display)
    {
        // The timeout as the power-up default -- saved straight away.
        if(events.btn2_released)
        {
            if(!PerformanceStore::IsCardPresent())
                PerformanceStore::Remount();
            g_audio_suspended = true; // see TriggerSave() -- a blocking SD write
            const bool ok = PerformanceStore::SaveScreenSaver((uint32_t)SaverMinutes(saver_option_));
            g_audio_suspended = false;
            if(ok)
                saver_default_ = saver_option_;
            snprintf(saver_status_, sizeof(saver_status_), ok ? "" : "NOT SAVED");
        }
    }
    else if(screen_ == Screen::Global && global_page_ == GlobalPage::Looper)
    {
        // Default On/Off for power-up -- saved straight away, separate
        // from B1's live On/Off.
        if(events.btn2_released)
        {
            if(!PerformanceStore::IsCardPresent())
                PerformanceStore::Remount();
            bool next         = !looper_default_on_;
            g_audio_suspended = true; // see TriggerSave() -- a blocking SD write
            bool ok           = PerformanceStore::SaveLooperDefault(next);
            g_audio_suspended = false;
            if(ok)
            {
                looper_default_on_ = next;
                looper_status_[0]  = '\0';
            }
            else
                snprintf(looper_status_, sizeof(looper_status_), "%s",
                          PerformanceStore::IsCardPresent() ? "Save failed" : "No SD card");
            button2_long_fired_ = false;
        }
    }
    else if(screen_ == Screen::Global && global_page_ == GlobalPage::Snap)
    {
        // Tap to open the list; inside it, a tap loads the highlighted
        // snapshot and stays in the list. Saving keeps Global:Perf's
        // 800 ms hold (it can overwrite).
        bool can_confirm_save = save_load_mode_ == SaveLoadMode::ChoosingSave;
        if(b.Pressed() && b.TimeHeldMs() > 800.f && !button2_long_fired_ && can_confirm_save)
        {
            button2_long_fired_ = true;
            TriggerSaveSnapshot(save_as_new_);
            save_load_mode_ = SaveLoadMode::Idle;
        }
        if(events.btn2_released)
        {
            if(button2_long_fired_)
            {
                // The hold above already saved -- this release ends it.
            }
            else if(can_confirm_save)
            {
                if(events.btn2_held_ms > 800.f)
                {
                    TriggerSaveSnapshot(save_as_new_);
                    save_load_mode_ = SaveLoadMode::Idle;
                }
            }
            else if(save_load_mode_ == SaveLoadMode::BrowsingLoad)
            {
                if(snap_cursor_ < snap_slot_count_)
                    TriggerLoadSnapshot(snap_slots_[snap_cursor_]);
            }
            else if(!PerformanceStore::IsCardPresent())
            {
                PerformanceStore::Remount();
                snap_slots_dirty_ = true;
            }
            else
            {
                save_load_mode_   = SaveLoadMode::BrowsingLoad;
                snap_slots_dirty_ = true;
                snap_status_[0]   = '\0';
            }
            button2_long_fired_ = false;
        }
    }
    else if(screen_ == Screen::Global && global_page_ == GlobalPage::File)
    {
        // Same convention as every engine's User page: saving keeps an
        // 800 ms hold (it can overwrite); loading is a tap that stays in
        // the list. Entry 0 of the list is "New (blank)".
        bool can_confirm_save = save_load_mode_ == SaveLoadMode::ChoosingSave;
        if(b.Pressed() && b.TimeHeldMs() > 800.f && !button2_long_fired_ && can_confirm_save)
        {
            button2_long_fired_ = true;
            TriggerSave(save_as_new_);
            save_load_mode_ = SaveLoadMode::Idle;
        }
        if(events.btn2_released)
        {
            if(!button2_long_fired_ && events.btn2_held_ms > 800.f && can_confirm_save)
            {
                TriggerSave(save_as_new_);
                save_load_mode_ = SaveLoadMode::Idle;
            }
            else if(!button2_long_fired_ && save_load_mode_ == SaveLoadMode::BrowsingLoad)
            {
                if(file_cursor_ == 0)
                    TriggerNew();
                else if(file_cursor_ - 1 < file_slot_count_)
                    TriggerLoad(file_slots_[file_cursor_ - 1]);
            }
            else if(!button2_long_fired_ && save_load_mode_ == SaveLoadMode::Idle)
            {
                if(!PerformanceStore::IsCardPresent())
                {
                    PerformanceStore::Remount();
                    file_slots_dirty_ = true; // re-scan once actually mounted
                }
                else
                    save_load_mode_ = SaveLoadMode::BrowsingLoad;
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
    else if(screen_ == Screen::Granular && granular_param_page_ == GranularParamPage::Trim)
    {
        // Hold to save the trimmed capture into the Preset page's
        // Captured folders -- same as SMPVO's own Trim page.
        if(b.Pressed() && b.TimeHeldMs() > 800.f && !button2_long_fired_)
        {
            button2_long_fired_ = true;
            TriggerSaveGranularCapture();
        }
        if(events.btn2_released)
        {
            if(!button2_long_fired_ && events.btn2_held_ms > 800.f)
                TriggerSaveGranularCapture();
            button2_long_fired_ = false;
        }
    }
    else if(screen_ == Screen::Granular && granular_param_page_ == GranularParamPage::Preset)
    {
        // Shared browse convention: tap opens the browser, then a folder;
        // a tap on a sound loads it and stays in the folder. No hold.
        bool at_folder_list = granular_lib_folder_ < 0;
        if(events.btn2_released)
        {
            if(save_load_mode_ == SaveLoadMode::BrowsingLoad && !at_folder_list)
            {
                if(granular_lib_count_ > 0)
                    LoadGranularLibraryItem();
            }
            else if(save_load_mode_ == SaveLoadMode::BrowsingLoad)
            {
                granular_lib_folder_      = granular_lib_folder_cursor_;
                granular_lib_items_dirty_ = true;
                granular_lib_cursor_      = 0;
            }
            else if(!PerformanceStore::IsCardPresent())
                PerformanceStore::Remount();
            else
            {
                save_load_mode_          = SaveLoadMode::BrowsingLoad;
                granular_lib_folder_     = -1; // always start at the folder list
                granular_lib_dirty_      = true;
                granular_preset_status_[0] = '\0';
            }
            button2_long_fired_ = false;
        }
    }
    else if(screen_ == Screen::Granular && granular_param_page_ == GranularParamPage::User)
    {
        // Saving keeps an 800 ms hold (it can overwrite); loading is a tap
        // that stays in the list. Entry 0 of the list is "New (blank)".
        bool can_confirm_save = save_load_mode_ == SaveLoadMode::ChoosingSave;
        if(b.Pressed() && b.TimeHeldMs() > 800.f && !button2_long_fired_ && can_confirm_save)
        {
            button2_long_fired_ = true;
            TriggerSaveGranularPreset(save_as_new_);
            save_load_mode_ = SaveLoadMode::Idle;
        }
        if(events.btn2_released)
        {
            if(!button2_long_fired_ && events.btn2_held_ms > 800.f && can_confirm_save)
            {
                TriggerSaveGranularPreset(save_as_new_);
                save_load_mode_ = SaveLoadMode::Idle;
            }
            else if(!button2_long_fired_ && save_load_mode_ == SaveLoadMode::BrowsingLoad)
            {
                if(granular_user_cursor_ == 0)
                    TriggerNewGranularPreset();
                else if(granular_user_cursor_ - 1 < granular_preset_user_slot_count_)
                    LoadGranularUserSlot(granular_preset_user_slots_[granular_user_cursor_ - 1]);
            }
            else if(!button2_long_fired_ && save_load_mode_ == SaveLoadMode::Idle)
            {
                if(!PerformanceStore::IsCardPresent())
                    PerformanceStore::Remount();
                else
                    save_load_mode_ = SaveLoadMode::BrowsingLoad;
            }
            button2_long_fired_ = false;
        }
    }
    else if(screen_ == Screen::Dexed && dexed_param_page_ == DexedParamPage::Preset)
    {
        // Shared browse convention: tap opens the browser, then a group,
        // then a folder; a tap on a preset loads it and stays in the
        // folder. No hold.
        if(events.btn2_released)
        {
            if(save_load_mode_ != SaveLoadMode::BrowsingLoad)
            {
                // Built-in presets need no card -- just try a remount in
                // case one was inserted, for the Imports group.
                if(!PerformanceStore::IsCardPresent())
                    PerformanceStore::Remount();
                {
                    save_load_mode_           = SaveLoadMode::BrowsingLoad;
                    dexed_files_group_open_   = false; // always start at the group list
                    dexed_preset_folder_open_ = false;
                    dexed_preset_slots_dirty_ = true;
                }
            }
            else if(!dexed_files_group_open_)
            {
                dexed_files_group_open_   = true;
                dexed_preset_folder_open_ = false;
                dexed_preset_slots_dirty_ = true;
            }
            else if(!dexed_preset_folder_open_)
            {
                dexed_preset_folder_open_ = true;
                dexed_preset_cursor_      = 0;
            }
            else
                LoadDexedSlot(ResolveDexedPresetSlot());
            button2_long_fired_ = false;
        }
    }
    else if(screen_ == Screen::Dexed && dexed_param_page_ == DexedParamPage::User)
    {
        // Saving keeps an 800 ms hold (it can overwrite); loading is a tap
        // that stays in the list. Entry 0 of the list is "New".
        bool can_confirm_save = save_load_mode_ == SaveLoadMode::ChoosingSave;
        if(b.Pressed() && b.TimeHeldMs() > 800.f && !button2_long_fired_ && can_confirm_save)
        {
            button2_long_fired_ = true;
            TriggerSaveDexedPreset(save_as_new_);
            save_load_mode_ = SaveLoadMode::Idle;
        }
        if(events.btn2_released)
        {
            if(!button2_long_fired_ && events.btn2_held_ms > 800.f && can_confirm_save)
            {
                TriggerSaveDexedPreset(save_as_new_);
                save_load_mode_ = SaveLoadMode::Idle;
            }
            else if(!button2_long_fired_ && save_load_mode_ == SaveLoadMode::BrowsingLoad)
            {
                if(dexed_user_cursor_ == 0)
                    TriggerNewDexedPreset();
                else if(dexed_user_cursor_ - 1 < dexed_preset_user_slot_count_)
                    LoadDexedSlot(dexed_preset_user_slots_[dexed_user_cursor_ - 1]);
            }
            else if(!button2_long_fired_ && save_load_mode_ == SaveLoadMode::Idle)
            {
                if(!PerformanceStore::IsCardPresent())
                    PerformanceStore::Remount();
                else
                    save_load_mode_ = SaveLoadMode::BrowsingLoad;
            }
            button2_long_fired_ = false;
        }
    }
    else if(screen_ == Screen::Dexed && dexed_param_page_ == DexedParamPage::Import)
    {
        // Hold-to-confirm, same as the Sampler's own Import page -- one
        // .syx can write up to 32 new presets.
        if(b.Pressed() && b.TimeHeldMs() > 800.f && !button2_long_fired_)
        {
            button2_long_fired_ = true;
            TriggerDexedSyxImport();
        }
        if(events.btn2_released)
        {
            if(!button2_long_fired_ && events.btn2_held_ms > 800.f)
                TriggerDexedSyxImport();
            else if(!button2_long_fired_ && !PerformanceStore::IsCardPresent())
            {
                PerformanceStore::Remount();
                dexed_import_files_dirty_ = true;
            }
            button2_long_fired_ = false;
        }
    }
    else if(screen_ == Screen::Ensemble && ensemble_param_page_ == EnsembleParamPage::Preset)
    {
        // Factory sounds, shared browse convention: tap to open the list,
        // tap a sound to load it and stay in the list.
        if(events.btn2_released)
        {
            if(save_load_mode_ == SaveLoadMode::BrowsingLoad)
                LoadEnsembleSlot(kEnsembleFactorySlotBase + ensemble_preset_cursor_);
            else
                save_load_mode_ = SaveLoadMode::BrowsingLoad;
            button2_long_fired_ = false;
        }
    }
    else if(screen_ == Screen::Ensemble && ensemble_param_page_ == EnsembleParamPage::User)
    {
        // Saving keeps an 800 ms hold (it can overwrite); loading is a tap
        // that stays in the list. Entry 0 of the list is "New (blank)".
        bool can_confirm_save = save_load_mode_ == SaveLoadMode::ChoosingSave;
        if(b.Pressed() && b.TimeHeldMs() > 800.f && !button2_long_fired_ && can_confirm_save)
        {
            button2_long_fired_ = true;
            TriggerSaveEnsemblePreset(save_as_new_);
            save_load_mode_ = SaveLoadMode::Idle;
        }
        if(events.btn2_released)
        {
            if(!button2_long_fired_ && events.btn2_held_ms > 800.f && can_confirm_save)
            {
                TriggerSaveEnsemblePreset(save_as_new_);
                save_load_mode_ = SaveLoadMode::Idle;
            }
            else if(!button2_long_fired_ && save_load_mode_ == SaveLoadMode::BrowsingLoad)
            {
                if(ensemble_user_cursor_ == 0)
                    TriggerNewEnsemblePreset();
                else if(ensemble_user_cursor_ - 1 < ensemble_preset_user_slot_count_)
                    LoadEnsembleSlot(ensemble_preset_user_slots_[ensemble_user_cursor_ - 1]);
            }
            else if(!button2_long_fired_ && save_load_mode_ == SaveLoadMode::Idle)
            {
                if(!PerformanceStore::IsCardPresent())
                    PerformanceStore::Remount();
                else
                    save_load_mode_ = SaveLoadMode::BrowsingLoad;
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
            if(!SampleSynthImportReady())
            {
                // SMPDR: a tap opens the selected type.
                sample_synth_import_drum_open_      = true;
                sample_synth_import_dest_cursor_    = 0; // New bank
                sample_synth_import_dirty_          = true;
                sample_synth_pack_import_status_[0] = '\0';
            }
            else if(!button2_long_fired_ && events.btn2_held_ms > 800.f)
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
            else if(save_load_mode_ == SaveLoadMode::BrowsingLoad
                    && AtSampleSynthVoiceTypeSelect())
            {
                // Open the chosen type (Sounds/Captured/Multi or
                // Kits/Breaks) -- its banks next.
                sample_synth_voice_type_            = sample_synth_voice_type_cursor_;
                sample_synth_sound_category_        = -1;
                sample_synth_sound_category_cursor_ = 0;
                sample_synth_banks_dirty_           = true;
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
                    sample_synth_voice_type_     = -1; // ...at the type list
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
                     && !is_voice && sample_synth_drum_user_folder_ < 0)
            {
                // SMPDR: open the highlighted Kits/Breaks folder.
                sample_synth_drum_user_folder_ = sample_synth_drum_user_folder_cursor_;
                int first = DrumUserGlobalIndex(sample_synth_drum_user_folder_, 0);
                if(first >= 0)
                    sample_synth_drum_user_cursor_ = first;
                status[0] = '\0';
            }
            else if(!button2_long_fired_ && save_load_mode_ == SaveLoadMode::BrowsingLoad
                     && count > 0)
            {
                if(is_voice)
                    TriggerLoadSampleSynthVoiceUserPreset();
                else if(sample_synth_drum_user_folder_count_[sample_synth_drum_user_folder_] > 0)
                    TriggerLoadSampleSynthDrumUserPreset();
            }
            else if(!button2_long_fired_ && save_load_mode_ == SaveLoadMode::Idle)
            {
                if(!PerformanceStore::IsCardPresent())
                    PerformanceStore::Remount();
                else
                {
                    save_load_mode_                = SaveLoadMode::BrowsingLoad;
                    sample_synth_drum_user_folder_ = -1; // SMPDR: start at Kits/Breaks
                    status[0]                      = '\0'; // clear any stale result
                }
            }
            button2_long_fired_ = false;
        }
    }
    else if(screen_ == Screen::SeqTrack && seq_page_ >= kSeqPageGrid)
    {
        // The grid's B2 is held to edit the note under the cursor -- read
        // in SeqGridKnobs(); a tap or release does nothing here.
        if(events.btn2_released)
            button2_long_fired_ = false;
    }
    else if(screen_ == Screen::Home && HomeScenes() && scene_edit_ >= 0)
    {
        // Editing: a tap turns the chosen layer/track on or off.
        if(events.btn2_released)
            SceneEditToggle();
    }
    else if(screen_ == Screen::Home && HomeScenes())
    {
        // Held 800ms: clear the chosen scene. A tap does nothing here.
        if(b.Pressed() && b.TimeHeldMs() > 800.f && !button2_long_fired_)
        {
            button2_long_fired_ = true;
            scenes_[scene_cursor_] = Scene{};
            if(scene_playing_ == scene_cursor_)
                scene_playing_ = -1;
        }
        if(events.btn2_released)
        {
            if(!button2_long_fired_ && events.btn2_held_ms > 800.f)
            {
                scenes_[scene_cursor_] = Scene{};
                if(scene_playing_ == scene_cursor_)
                    scene_playing_ = -1;
            }
            button2_long_fired_ = false;
        }
    }
    else if(screen_ == Screen::Home && HomeSong())
    {
        // Tap: insert a copy of the step after it (or add one at the end).
        // Held 800ms: delete the step.
        auto del = [&] {
            if(song_cursor_ >= song_.len)
                return;
            for(int k = song_cursor_; k < song_.len - 1; k++)
                song_.steps[k] = song_.steps[k + 1];
            song_.len--;
        };
        if(b.Pressed() && b.TimeHeldMs() > 800.f && !button2_long_fired_)
        {
            button2_long_fired_ = true;
            del();
        }
        if(events.btn2_released)
        {
            if(!button2_long_fired_)
            {
                if(events.btn2_held_ms > 800.f)
                    del();
                else if(song_.len < Song::kSteps)
                {
                    const int at = song_cursor_ < song_.len ? song_cursor_ + 1 : song_.len;
                    for(int k = song_.len; k > at; k--)
                        song_.steps[k] = song_.steps[k - 1];
                    song_.steps[at] = at > 0 ? song_.steps[at - 1] : SongStep{};
                    song_.len++;
                    song_cursor_ = at;
                }
            }
            button2_long_fired_ = false;
        }
    }
    else if(screen_ == Screen::SeqTrack && seq_page_ == kSeqPageChain)
    {
        // Tap: add the chosen pattern to the end of the chain. Held 800ms:
        // clear the chain (the track carries on with the pattern playing).
        auto clear = [&] {
            if(seq_)
                seq_->RequestChain(HomeSeqTrack(), nullptr, 0);
        };
        if(b.Pressed() && b.TimeHeldMs() > 800.f && !button2_long_fired_)
        {
            button2_long_fired_ = true;
            clear();
        }
        if(events.btn2_released)
        {
            if(!button2_long_fired_ && seq_)
            {
                if(events.btn2_held_ms > 800.f)
                    clear();
                else
                {
                    int8_t ch[MidiSequencer::kMaxChain];
                    int    n = seq_->GetChain(HomeSeqTrack(), ch);
                    if(n < MidiSequencer::kMaxChain)
                    {
                        ch[n++] = (int8_t)seq_patt_cursor_;
                        seq_->RequestChain(HomeSeqTrack(), ch, n);
                    }
                }
            }
            button2_long_fired_ = false;
        }
    }
    else if(screen_ == Screen::SeqTrack && seq_page_ == kSeqPagePatt)
    {
        // Tap: play the chosen pattern (from the end of the one playing).
        // Held 800ms: copy the playing pattern into the chosen one and
        // switch to it -- the start of a variation.
        auto copy = [&] {
            const int t = HomeSeqTrack();
            seq_->RequestCopyPattern(t, seq_patt_cursor_);
            if(arp_) // its arp goes with it
                arp_->CopyPattern(t, seq_->GetPattern(t), seq_patt_cursor_);
        };
        if(b.Pressed() && b.TimeHeldMs() > 800.f && !button2_long_fired_)
        {
            button2_long_fired_ = true;
            if(seq_)
                copy();
        }
        if(events.btn2_released)
        {
            if(!button2_long_fired_ && seq_)
            {
                if(events.btn2_held_ms > 800.f)
                    copy();
                else
                    seq_->RequestPattern(HomeSeqTrack(), seq_patt_cursor_);
            }
            button2_long_fired_ = false;
        }
    }
    else if(screen_ == Screen::SeqTrack && seq_page_ == kSeqPagePrint)
    {
        // Held 800ms: print. While printing, a tap stops it (cancels the
        // count-in, or ends the pass early) -- the layer's own B1 tap.
        if(b.Pressed() && b.TimeHeldMs() > 800.f && !button2_long_fired_)
        {
            button2_long_fired_ = true;
            if(seq_print_layer_ < 0)
                StartSeqPrint(HomeSeqTrack());
        }
        if(events.btn2_released)
        {
            if(!button2_long_fired_)
            {
                if(events.btn2_held_ms > 800.f && seq_print_layer_ < 0)
                    StartSeqPrint(HomeSeqTrack());
                else if(seq_print_layer_ >= 0)
                    layers_[seq_print_layer_].OnRecordButtonPressed(*tempo_);
            }
            button2_long_fired_ = false;
        }
    }
    else if(screen_ == Screen::SeqTrack && seq_page_ == kSeqPageArp)
    {
        // Held 800ms: this pattern's arp to every pattern of the track.
        auto all = [&] {
            if(arp_ && HomeSeqTrack() >= 0)
                arp_->CopyToAllPatterns(HomeSeqTrack(), seq_patt_cursor_);
        };
        if(b.Pressed() && b.TimeHeldMs() > 800.f && !button2_long_fired_)
        {
            button2_long_fired_ = true;
            all();
        }
        if(events.btn2_released)
        {
            if(!button2_long_fired_ && events.btn2_held_ms > 800.f)
                all();
            button2_long_fired_ = false;
        }
    }
    else if(screen_ == Screen::SeqTrack)
    {
        // As on a layer's Status page: tap = undo the last pass, held
        // 800ms = clear the track.
        if(b.Pressed() && b.TimeHeldMs() > 800.f && !button2_long_fired_)
        {
            button2_long_fired_ = true;
            if(seq_)
                seq_->RequestClear(HomeSeqTrack());
        }
        if(events.btn2_released)
        {
            if(!button2_long_fired_ && seq_)
            {
                if(events.btn2_held_ms > 800.f)
                    seq_->RequestClear(HomeSeqTrack());
                else
                    seq_->RequestUndo(HomeSeqTrack());
            }
            button2_long_fired_ = false;
        }
    }
    else if(screen_ == Screen::Global && global_page_ == GlobalPage::Midi)
    {
        // Button2's only job here: held 800ms, save every engine's
        // channel as the power-on default (same hold as Global:Tempo's).
        if(b.Pressed() && b.TimeHeldMs() > 800.f && !button2_long_fired_)
        {
            button2_long_fired_ = true;
            TriggerSaveMidiDefaults();
        }
        if(events.btn2_released)
        {
            if(!button2_long_fired_ && events.btn2_held_ms > 800.f)
                TriggerSaveMidiDefaults();
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
    else if(screen_ == Screen::Supersaw
            && (supersaw_page_ == SupersawParamPage::Preset
                || supersaw_page_ == SupersawParamPage::User))
        SupersawHandleButton2(events);
    else if(events.btn2_released)
    {
        OnButton2Short();
    }
}

void Ui::OnButton1Short()
{
    switch(screen_)
    {
        case Screen::Home:
            if(HomeScenes())
                LaunchScene(scene_edit_ >= 0 ? scene_edit_ : scene_cursor_);
            else if(HomeSong())
            {
                if(song_rec_)
                    StopSongRecord();
                else if(song_playing_)
                    StopSong();
                else
                    StartSong(song_cursor_);
            }
            else if(HomeSeqTrack() >= 0)
            {
                if(seq_)
                    seq_->RequestTap(HomeSeqTrack());
            }
            else
                layers_[cursor_layer_].OnRecordButtonPressed(*tempo_);
            break;
        case Screen::SeqTrack:
            if(seq_page_ >= kSeqPageGrid)
                SeqGridToggle(HomeSeqTrack()); // the grid: add/remove at the cursor
            else if(seq_page_ == kSeqPageArp && arp_ && HomeSeqTrack() >= 0)
            {
                // The chosen pattern's arp on/off.
                Arpeggiator::Settings& s = arp_->PatternSettings(HomeSeqTrack(), seq_patt_cursor_);
                s.on                     = !s.on;
            }
            else if(seq_)
                seq_->RequestTap(HomeSeqTrack());
            break;
        case Screen::Layer:
            switch(layer_page_)
            {
                case LayerPage::Status: Cur().OnRecordButtonPressed(*tempo_); break;
                case LayerPage::Filter:
                {
                    int n = (int)FilterMode::kNumModes;
                    int m = ((int)Cur().GetFilterMode() + 1) % n;
                    Cur().SetFilterMode((FilterMode)m);
                    break;
                }
                default: break;
            }
            break;
        case Screen::Global:
            if(global_page_ == GlobalPage::Tempo)
                tempo_->ToggleMetronome();
            else if(global_page_ == GlobalPage::Midi)
                SetPedalReversed(!pedal_reversed_); // Norm <-> Rev
            else if(global_page_ == GlobalPage::MixTracks || global_page_ == GlobalPage::MixSynths)
                mixer_profile_view_ = !mixer_profile_view_;
            else if(global_page_ == GlobalPage::Arp && arp_)
                arp_->Get(arp_engine_).on = !arp_->Get(arp_engine_).on;
            else if(global_page_ == GlobalPage::Sends)
            {
                sends_target_delay_ = false; // K2 -> Reverb
                k2_pickup_engaged_[(size_t)KnobContext::GlobalSends] = false;
                SyncPickupTargets(KnobContext::GlobalSends);
            }
            else if(global_page_ == GlobalPage::Filter)
            {
                int n = (int)FilterMode::kNumModes;
                int m = ((int)master_filter_mode_ + 1) % n;
                master_filter_mode_ = (FilterMode)m;
            }
            else if(global_page_ == GlobalPage::Bypass)
                bypass_fx_target_delay_ = false; // knobs -> Reverb (Send+Size)
            else if(global_page_ == GlobalPage::Delay)
                delay_sync_ = false; // Time: free ms (B2 = note values)
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
                if(save_load_mode_ != SaveLoadMode::Idle)
                    save_load_mode_ = SaveLoadMode::Idle; // Back
                // No card: this hand-wired socket has no card-detect pin
                // (see PerformanceStore::Remount()'s doc comment), so a
                // card swapped out and back in needs an explicit re-mount.
                else if(!PerformanceStore::IsCardPresent())
                {
                    PerformanceStore::Remount();
                    file_slots_dirty_ = true; // re-scan once actually mounted
                }
                else
                {
                    // Reveal Overwrite / Save New -- K1 picks, holding
                    // Button2 confirms. Overwrite only if something's loaded.
                    save_load_mode_ = SaveLoadMode::ChoosingSave;
                    save_as_new_    = loaded_slot_ < 0;
                }
            }
            else if(global_page_ == GlobalPage::Snap)
            {
                if(save_load_mode_ != SaveLoadMode::Idle)
                    save_load_mode_ = SaveLoadMode::Idle; // Back
                else if(!PerformanceStore::IsCardPresent())
                {
                    PerformanceStore::Remount();
                    snap_slots_dirty_ = true;
                }
                else
                {
                    // Reveal Overwrite / Save New -- K1 picks, holding
                    // Button2 confirms, same as Global:Perf.
                    save_load_mode_ = SaveLoadMode::ChoosingSave;
                    save_as_new_    = snap_loaded_slot_ < 0;
                }
            }
            else if(global_page_ == GlobalPage::Export)
            {
                if(!PerformanceStore::IsCardPresent())
                    PerformanceStore::Remount();
                else if(live_open_)
                    StopLiveExport();
                else if(export_mode_ == kExportSong)
                    StartSongExport(false);
                else if(export_mode_ == kExportLive)
                    StartLiveExport(false);
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
            else if(global_page_ == GlobalPage::Dexed || global_page_ == GlobalPage::DexedB)
            {
                int  engine = global_page_ == GlobalPage::DexedB ? 1 : 0;
                bool& on    = engine == 1 ? dexed_b_enabled_ : dexed_enabled_;
                on          = !on;
                if(dexed_engines_[engine])
                    dexed_engines_[engine]->AllNotesOff();
                // The engine already running keeps its voices; the one
                // just switched on gets what's left of the pair's 24.
                if(on)
                    ApplyDexedVoiceCeiling(1 - engine);
            }
            else if(global_page_ == GlobalPage::Ensemble)
            {
                ensemble_enabled_ = !ensemble_enabled_;
                if(ensemble_engines_[0])
                    ensemble_engines_[0]->AllNotesOff();
            }
            else if(global_page_ == GlobalPage::Organ)
            {
                organ_enabled_ = !organ_enabled_;
                if(ensemble_engines_[1])
                    ensemble_engines_[1]->AllNotesOff();
            }
            else if(global_page_ == GlobalPage::Supersaw)
            {
                supersaw_enabled_ = !supersaw_enabled_;
                if(supersaw_)
                    supersaw_->AllNotesOff();
            }
            else if(global_page_ == GlobalPage::SampleSynthVoice)
            {
                sample_synth_voice_enabled_ = !sample_synth_voice_enabled_;
                if(sample_synth_engines_[0])
                    sample_synth_engines_[0]->AllNotesOff();
            }
            else if(global_page_ == GlobalPage::SampleSynthDrum)
            {
                sample_synth_drum_enabled_ = !sample_synth_drum_enabled_;
                if(sample_synth_engines_[1])
                    sample_synth_engines_[1]->AllNotesOff();
            }
            else if(global_page_ == GlobalPage::Display)
            {
                // Sleep now (on the release, so it doesn't wake straight back up).
                SetSaver(Saver::Off);
                saver_touch_ms_ = System::GetNow();
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
            else if(global_page_ == GlobalPage::SdMgmt && sd_mgmt_in_folder_)
            {
                // Back up one level: items -> bank list -> (CAPT only)
                // Sampler/Grains -> the box grid. K1 hasn't moved, so the
                // same folder is still highlighted. B1's hold means
                // Duplicate in an item list (see OnButton1Long()).
                if(SdMgmtIsBankFolder() && sd_mgmt_bank_ >= 0)
                    sd_mgmt_bank_ = -1;
                else if(SdMgmtDrumUserBox() && sd_mgmt_drum_kind_ >= 0)
                    sd_mgmt_drum_kind_ = -1;
                else if(SdMgmtGlobalBox() && sd_mgmt_global_kind_ >= 0)
                    sd_mgmt_global_kind_ = -1;
                else if(SdMgmtEnsembleBox() && sd_mgmt_ens_kind_ >= 0)
                    sd_mgmt_ens_kind_ = -1;
                else if(sd_mgmt_folder_ == SdMgmtFolder::SampleSynthImports
                        && sd_mgmt_smp_kind_ >= 0)
                {
                    // bank list -> Sounds/Multi or Kits/Breaks -> Voice/Drum
                    if(sd_mgmt_smp_sub_kind_ >= 0)
                        sd_mgmt_smp_sub_kind_ = -1;
                    else
                        sd_mgmt_smp_kind_ = -1;
                    sd_mgmt_banks_dirty_ = true;
                }
                else if(sd_mgmt_folder_ == SdMgmtFolder::SampleSynthCaptures
                        && sd_mgmt_capt_kind_ >= 0)
                {
                    sd_mgmt_capt_kind_ = -1;
                    sd_mgmt_banks_dirty_ = true;
                }
                else
                    sd_mgmt_in_folder_ = false;
                sd_mgmt_status_[0] = '\0';
            }
            break;
        case Screen::Granular:
            if(granular_param_page_ == GranularParamPage::Mix)
                granular_mix_target_gain_ = false; // knobs -> Level+Pan
            else if(granular_param_page_ == GranularParamPage::FX)
                granular_fx_target_delay_ = false; // knobs -> Reverb (Send+Size)
            else if(granular_param_page_ == GranularParamPage::ADSR)
                granular_adsr_target_sr_ = false; // knobs -> Attack+Decay
            else if(granular_param_page_ == GranularParamPage::Pitch && granular_)
                granular_->SetGrainFollowsNote(!granular_->GetGrainFollowsNote());
            else if(granular_param_page_ == GranularParamPage::Scan && granular_)
                granular_->CycleScanMode();
            else if(granular_param_page_ == GranularParamPage::Rhythm && granular_)
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
                // Back one level: a folder's sounds -> the folder list (K1
                // still where it was, so the same folder stays highlighted)
                // -> out of the browser.
                if(save_load_mode_ != SaveLoadMode::Idle)
                {
                    if(granular_lib_folder_ >= 0)
                        granular_lib_folder_ = -1;
                    else
                        save_load_mode_ = SaveLoadMode::Idle;
                }
                else if(!PerformanceStore::IsCardPresent())
                    PerformanceStore::Remount();
            }
            else if(granular_param_page_ == GranularParamPage::User)
            {
                if(save_load_mode_ != SaveLoadMode::Idle)
                    save_load_mode_ = SaveLoadMode::Idle; // Back
                else if(!PerformanceStore::IsCardPresent())
                    PerformanceStore::Remount();
                else
                {
                    // Reveal Overwrite / Save New -- K1 picks, holding
                    // Button2 confirms (see HandleButton2()).
                    save_load_mode_ = SaveLoadMode::ChoosingSave;
                    save_as_new_    = granular_loaded_preset_slot_ <= 0;
                }
            }
            break;
        case Screen::Dexed:
            if(dexed_param_page_ == DexedParamPage::Filter && dexed_)
            {
                // Same "Button1 cycles" idiom as Layer:Filter/Granular:Filter.
                int n = (int)FilterMode::kNumModes;
                int m = ((int)dexed_->GetFilterMode() + 1) % n;
                dexed_->SetFilterMode((FilterMode)m);
            }
            else if(dexed_param_page_ == DexedParamPage::FX)
                dexed_fx_target_delay_ = false; // knobs -> Reverb (Send+Size)
            else if(dexed_param_page_ == DexedParamPage::Mix)
            {
                // Out of Gain first; after that each tap moves to the
                // next bar, A -> B -> Bus.
                if(dexed_mix_target_gain_)
                    dexed_mix_target_gain_ = false;
                else
                    dexed_mix_sel_ = (dexed_mix_sel_ + 1) % 3;
            }
            else if(dexed_param_page_ == DexedParamPage::Split)
                dexed_split_ = !dexed_split_; // Layer <-> Split
            else if(dexed_param_page_ == DexedParamPage::MIDI)
            {
                // Lock: B follows A's channel. Turning it off gives B its
                // own, starting from A's so nothing changes until you
                // set it.
                dexed_midi_lock_ = !dexed_midi_lock_;
                if(!dexed_midi_lock_)
                    dexed_b_midi_channel_ = dexed_midi_channel_;
                ResyncKnobs();
            }
            else if(dexed_param_page_ == DexedParamPage::Vibrato && dexed_)
                dexed_->CycleModWheelTarget();
            else if(dexed_param_page_ == DexedParamPage::Preset)
            {
                // Back one level: preset list -> folder list -> group list
                // -> out of the browser. K1 hasn't moved, so the same
                // folder/group stays highlighted.
                if(save_load_mode_ == SaveLoadMode::Idle)
                {
                    if(!PerformanceStore::IsCardPresent())
                        PerformanceStore::Remount();
                }
                else if(dexed_preset_folder_open_)
                    dexed_preset_folder_open_ = false;
                else if(dexed_files_group_open_)
                    dexed_files_group_open_ = false;
                else
                    save_load_mode_ = SaveLoadMode::Idle;
            }
            else if(dexed_param_page_ == DexedParamPage::User)
            {
                if(save_load_mode_ != SaveLoadMode::Idle)
                    save_load_mode_ = SaveLoadMode::Idle; // Back
                else if(!PerformanceStore::IsCardPresent())
                    PerformanceStore::Remount();
                else
                {
                    // Reveal Overwrite / Save New -- K1 picks, holding
                    // Button2 confirms (see HandleButton2()).
                    save_load_mode_ = SaveLoadMode::ChoosingSave;
                    save_as_new_
                        = dexed_loaded_preset_slot_ <= DexedSynth::GetNumFactoryPresets();
                }
            }
            break;
        case Screen::Supersaw: SupersawButton1(); break;
        case Screen::Ensemble:
            if(ensemble_param_page_ == EnsembleParamPage::Perc && ensemble_)
                ensemble_->SetPercOn(!ensemble_->GetPercOn());
            else if(ensemble_param_page_ == EnsembleParamPage::Lfo && ensemble_)
                LfoCycleWave(ensemble_->Lfo());
            else if(ensemble_param_page_ == EnsembleParamPage::Env)
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
                if(save_load_mode_ != SaveLoadMode::Idle)
                    save_load_mode_ = SaveLoadMode::Idle; // Back
            }
            else if(ensemble_param_page_ == EnsembleParamPage::User)
            {
                if(save_load_mode_ != SaveLoadMode::Idle)
                    save_load_mode_ = SaveLoadMode::Idle; // Back
                else if(!PerformanceStore::IsCardPresent())
                    PerformanceStore::Remount();
                else
                {
                    // Reveal Overwrite / Save New -- K1 picks, holding
                    // Button2 confirms (see HandleButton2()).
                    save_load_mode_ = SaveLoadMode::ChoosingSave;
                    save_as_new_    = !EnsembleIsUserSlot(ensemble_loaded_preset_slot_);
                }
            }
            break;
        case Screen::DexedOperator:
        {
            if(IsDexedVoicePage(dexed_op_page_))
                break; // voice-wide pages -- no operator to select
            // Button1 cycles which of the 6 operators is being edited --
            // rotate stays reserved for DexedOpParamPage navigation, same
            // "Button1 cycles a non-page selector" convention as Algo's
            // own algorithm cycle above.
            // OP1 -> OP6 order: array index 5 is OP1, so step downward.
            dexed_op_index_ = (dexed_op_index_ + 5) % 6;
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
        case Screen::Arp:
            if(arp_)
            {
                // Setup: Generator/Phrase. Pattern: the next shape
                // (Generator). Key: Follow/Lock. (Feel's Var -/+ are
                // HandleArpVariationButton()'s; on/off is Global:Arp's.)
                Arpeggiator::Settings& s = ArpEdit();
                if(arp_page_ == 0)
                    s.mode = s.mode == Arpeggiator::kGenerator ? Arpeggiator::kPhrase
                                                               : Arpeggiator::kGenerator;
                else if(arp_page_ == 1 && s.mode == Arpeggiator::kGenerator)
                    s.shape = (uint8_t)((s.shape + 1) % Arpeggiator::kNumShapes);
                else if(arp_page_ == 3)
                    s.lock = !s.lock;
            }
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
            else if(sample_synth_param_page_ == SampleSynthParamPage::Import
                    && sample_synth_import_drum_open_)
            {
                sample_synth_import_drum_open_      = false; // back to Kits/Breaks
                sample_synth_import_dirty_          = true;
                sample_synth_pack_import_status_[0] = '\0';
            }
            else if(sample_synth_param_page_ == SampleSynthParamPage::Env)
                sample_synth_adsr_target_sr_ = false; // knobs -> Attack+Decay
            else if(sample_synth_param_page_ == SampleSynthParamPage::FX)
                sample_synth_fx_target_delay_ = false; // knobs -> Reverb (Send+Size)
            else if(sample_synth_param_page_ == SampleSynthParamPage::Mix)
                sample_synth_mix_target_gain_ = false; // knobs -> Level+Pan
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
            else if(sample_synth_param_page_ == SampleSynthParamPage::Preset)
            {
                if(save_load_mode_ != SaveLoadMode::Idle)
                {
                    // Back one level: a folder's sounds -> the folder
                    // list (K1 still where it was, so the same folder
                    // stays highlighted) -> out of the browser.
                    if(sample_synth_sound_category_ >= 0)
                        sample_synth_sound_category_ = -1;
                    else if(sample_synth_voice_type_ >= 0)
                        sample_synth_voice_type_ = -1; // banks -> the type list
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
                if(save_load_mode_ == SaveLoadMode::BrowsingLoad
                   && sample_synth_mode_ == SampleSynthMode::Drum
                   && sample_synth_drum_user_folder_ >= 0)
                {
                    sample_synth_drum_user_folder_ = -1; // Back to Kits/Breaks, same folder highlighted
                }
                else if(save_load_mode_ != SaveLoadMode::Idle)
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
    if(screen_ == Screen::SeqTrack && seq_page_ >= kSeqPageGrid)
        ; // the grid's B1 is a tap only
    else if(screen_ == Screen::Home && HomeScenes())
        SaveScene(scene_edit_ >= 0 ? scene_edit_ : scene_cursor_);
    else if(screen_ == Screen::Home && HomeSong())
    {
        if(!song_rec_)
            StartSongRecord();
    }
    else if((screen_ == Screen::Home || screen_ == Screen::SeqTrack) && HomeSeqTrack() >= 0)
        SeqRecord(HomeSeqTrack()); // hold = record/overdub, as on a layer
    else if(screen_ == Screen::Home)
        layers_[cursor_layer_].OnRecordButtonLongPress(*tempo_);
    else if(screen_ == Screen::Layer && layer_page_ == LayerPage::Status)
        Cur().OnRecordButtonLongPress(*tempo_);
    else if(screen_ == Screen::Global && global_page_ == GlobalPage::SdMgmt
            && sd_mgmt_in_folder_ && !SdMgmtAtFolderList())
        TriggerSdMgmtDuplicate();
}

void Ui::OnButton1Release()
{
    if(screen_ == Screen::Home && HomeSeqTrack() < 0 && !HomeScenes() && !HomeSong())
        layers_[cursor_layer_].OnRecordButtonReleased();
    else if(screen_ == Screen::Layer && layer_page_ == LayerPage::Status)
        Cur().OnRecordButtonReleased();
}

void Ui::OnButton2Short()
{
    if(screen_ == Screen::Home)
        bypass_ = !bypass_;
    else if(screen_ == Screen::Global && global_page_ == GlobalPage::Bypass)
        bypass_fx_target_delay_ = true; // knobs -> Delay (Send+Time)
    else if(screen_ == Screen::Global && global_page_ == GlobalPage::Arp && arp_)
    {
        Arpeggiator::Settings& s = arp_->Get(arp_engine_); // Generator <-> Phrase
        s.mode = s.mode == Arpeggiator::kGenerator ? Arpeggiator::kPhrase : Arpeggiator::kGenerator;
    }
    else if(screen_ == Screen::Global && global_page_ == GlobalPage::Sends)
    {
        sends_target_delay_ = true; // K2 -> Delay
        k2_pickup_engaged_[(size_t)KnobContext::GlobalSends] = false;
        SyncPickupTargets(KnobContext::GlobalSends);
    }
    else if(screen_ == Screen::Global && global_page_ == GlobalPage::Delay)
        delay_sync_ = true; // Time: note values (B1 = free ms)
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
    {
        if(live_open_)
            StopLiveExport();
        else if(export_mode_ == kExportSong && PerformanceStore::IsCardPresent())
            StartSongExport(true);
        else if(export_mode_ == kExportLive && PerformanceStore::IsCardPresent())
            StartLiveExport(true);
        else
            TriggerExportMicroDexed();
    }
    else if(screen_ == Screen::Dexed && dexed_param_page_ == DexedParamPage::FX)
        dexed_fx_target_delay_ = true; // knobs -> Delay (Send+Time)
    else if(screen_ == Screen::Dexed && dexed_param_page_ == DexedParamPage::Split)
    {
        dexed_split_oct_b_ = !dexed_split_oct_b_; // K2 shifts A or B
        ResyncKnobs();
    }
    else if(screen_ == Screen::Dexed && dexed_param_page_ == DexedParamPage::Mix)
        dexed_mix_target_gain_ = !dexed_mix_target_gain_; // knobs -> bus Gain, and back
    else if(screen_ == Screen::Dexed && dexed_param_page_ == DexedParamPage::Vibrato)
        // Toggles (not just sets true) -- same reasoning as
        // Screen::DexedOperator's own Button2 toggles: Button1 on this
        // page is already committed to cycling the mod wheel's target,
        // so there's no second button free to select Speed/Depth
        // explicitly; Button2 alone has to cover both directions.
        dexed_vibrato_target_sens_ = !dexed_vibrato_target_sens_;
    else if(screen_ == Screen::DexedOperator)
        OnDexedOperatorButton2();
    else if(screen_ == Screen::Granular && granular_param_page_ == GranularParamPage::Rhythm
            && granular_)
        granular_->CycleGrainSpeed();
    else if(screen_ == Screen::Granular && granular_param_page_ == GranularParamPage::Scan
            && granular_)
        granular_->CycleDirection();
    else if(screen_ == Screen::Granular && granular_param_page_ == GranularParamPage::Pitch
            && granular_)
        granular_->SetPoly(!granular_->GetPoly());
    else if(screen_ == Screen::Granular && granular_param_page_ == GranularParamPage::ADSR)
        granular_adsr_target_sr_ = true; // knobs -> Sustain/Release
    else if(screen_ == Screen::Granular && granular_param_page_ == GranularParamPage::Mix)
        granular_mix_target_gain_ = true; // knob -> Gain
    else if(screen_ == Screen::Granular && granular_param_page_ == GranularParamPage::FX)
        granular_fx_target_delay_ = true; // knobs -> Delay (Send+Time)
    else if(screen_ == Screen::Ensemble && ensemble_param_page_ == EnsembleParamPage::Perc
            && ensemble_)
        ensemble_->SetPercThird(!ensemble_->GetPercThird()); // 2nd <-> 3rd harmonic
    else if(screen_ == Screen::Ensemble && ensemble_param_page_ == EnsembleParamPage::Env)
        ensemble_env_target_sr_ = true; // knobs -> Sustain+Release
    else if(screen_ == Screen::Ensemble && ensemble_param_page_ == EnsembleParamPage::FX)
        ensemble_fx_target_delay_ = true; // knobs -> Delay (Send+Time)
    else if(screen_ == Screen::Supersaw)
        SupersawButton2Short();
    else if(screen_ == Screen::Ensemble && ensemble_param_page_ == EnsembleParamPage::Lfo
            && ensemble_)
        LfoToggleSync(ensemble_->Lfo(), KnobContext::EnsembleLfo);
    else if(screen_ == Screen::Ensemble && ensemble_param_page_ == EnsembleParamPage::MIDI
            && ensemble_ && !ensemble_->IsOrgan())
        ensemble_midi_vel_ = !ensemble_midi_vel_; // knobs -> Velocity / Channel+Voices
    else if(screen_ == Screen::SampleSynth && sample_synth_param_page_ == SampleSynthParamPage::Env)
        sample_synth_adsr_target_sr_ = true; // knobs -> Sustain+Release
    else if(screen_ == Screen::SampleSynth && sample_synth_param_page_ == SampleSynthParamPage::FX)
        sample_synth_fx_target_delay_ = true; // knobs -> Delay (Send+Time)
    else if(screen_ == Screen::SampleSynth && sample_synth_param_page_ == SampleSynthParamPage::Mix)
        sample_synth_mix_target_gain_ = true; // knobs -> Gain
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
        mixer_target_reverb_ = true; // knobs -> Reverb + Delay Send (ignored on Master)
    else if(screen_ == Screen::Arp && arp_)
    {
        // Setup: latch on/off. Pattern: Repeat 1..4 (Generator). Key: the
        // next chord.
        Arpeggiator::Settings& s = ArpEdit();
        if(arp_page_ == 0)
            s.latch = !s.latch;
        else if(arp_page_ == 1 && s.mode == Arpeggiator::kGenerator)
            s.repeat = (uint8_t)(s.repeat % Arpeggiator::kMaxRepeat + 1);
        else if(arp_page_ == 3)
            s.chord = Arpeggiator::NextChord(s.chord);
    }
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
        case Screen::Home:
            return HomeScenes() && scene_edit_ >= 0 ? KnobContext::HomeSceneEdit
                   : HomeScenes()            ? KnobContext::HomeScenes
                   : HomeSong() && song_edit_ ? KnobContext::HomeSongEdit
                   : HomeSong()            ? KnobContext::HomeSong
                                           : KnobContext::Home;
        case Screen::SeqTrack:
            return seq_page_ >= kSeqPageGrid    ? KnobContext::SeqGrid
                   : seq_page_ == kSeqPagePrint ? KnobContext::SeqPrint
                   : seq_page_ == kSeqPagePatt  ? KnobContext::SeqPatt
                   : seq_page_ == kSeqPageChain ? KnobContext::SeqChain
                   : seq_page_ == kSeqPageArp   ? KnobContext::SeqArp
                   : seq_page_ == kSeqPageQuant ? KnobContext::SeqQuant
                                                : KnobContext::SeqTrack;
        case Screen::Layer:
            switch(layer_page_)
            {
                case LayerPage::Status: return KnobContext::LayerStatus;
                case LayerPage::Filter: return KnobContext::LayerFilter;
                case LayerPage::Drive: return KnobContext::LayerDrive;
                case LayerPage::Tremolo: return KnobContext::LayerTremolo;
                case LayerPage::Reverb: return KnobContext::LayerReverb;
                case LayerPage::Gain: return KnobContext::LayerGain;
                default: return KnobContext::LayerStatus;
            }
        case Screen::Global:
            switch(global_page_)
            {
                case GlobalPage::Tempo: return KnobContext::GlobalTempo;
                case GlobalPage::Filter: return KnobContext::GlobalFilter;
                case GlobalPage::Reverb: return KnobContext::GlobalReverb;
                case GlobalPage::Delay: return KnobContext::GlobalDelay;
                case GlobalPage::DelayReverb: return KnobContext::GlobalDelayReverb;
                case GlobalPage::DelayTone: return KnobContext::GlobalDelayTone;
                case GlobalPage::Bypass:
                    return bypass_fx_target_delay_ ? KnobContext::GlobalBypassDelay
                                                      : KnobContext::GlobalBypassReverb;
                case GlobalPage::ReverbTone:
                    return global_reverb_tone_target_b_
                               ? KnobContext::GlobalReverbToneDecayDamping
                               : KnobContext::GlobalReverbTonePreDelay;
                case GlobalPage::ReverbDiffusion: return KnobContext::GlobalReverbDiffusion;
                case GlobalPage::Speed: return KnobContext::GlobalSpeed;
                case GlobalPage::File:
                case GlobalPage::Snap: return KnobContext::GlobalFile; // list browse, no pickup
                case GlobalPage::Export: return KnobContext::GlobalExport;
                case GlobalPage::Midi: return KnobContext::GlobalMidi;
                case GlobalPage::Arp: return KnobContext::GlobalArp;
                case GlobalPage::Granular: return KnobContext::GlobalGranular;
                case GlobalPage::Looper: return KnobContext::GlobalLooper;
                case GlobalPage::Display: return KnobContext::GlobalDisplay;
                case GlobalPage::MixTracks:
                case GlobalPage::MixSynths: return KnobContext::GlobalMixer;
                case GlobalPage::Sends: return KnobContext::GlobalSends;
                case GlobalPage::SdMgmt: return KnobContext::GlobalSdMgmt;
                case GlobalPage::Dexed:
                case GlobalPage::DexedB: return KnobContext::GlobalDexed;
                case GlobalPage::Ensemble:
                case GlobalPage::Organ:
                case GlobalPage::Supersaw: return KnobContext::GlobalEnsemble;
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
                case DexedParamPage::Mix:
                    if(dexed_mix_target_gain_)
                        return KnobContext::DexedMixGain;
                    return dexed_mix_sel_ == 0   ? KnobContext::DexedMixA
                           : dexed_mix_sel_ == 1 ? KnobContext::DexedMixB
                                                 : KnobContext::DexedMix;
                case DexedParamPage::FX:
                    return dexed_fx_target_delay_ ? KnobContext::DexedFxDelay
                                                     : KnobContext::DexedFxReverb;
                case DexedParamPage::Fold: return KnobContext::DexedFold;
                case DexedParamPage::MIDI: return KnobContext::DexedMidi;
                case DexedParamPage::Split: return KnobContext::DexedSplit;
                case DexedParamPage::Advanced: return KnobContext::DexedAdvanced;
                case DexedParamPage::Preset: return KnobContext::DexedPreset;
                case DexedParamPage::User: return KnobContext::DexedUser;
                case DexedParamPage::Import: return KnobContext::DexedImport;
                default: return KnobContext::DexedAlgo;
            }
        case Screen::DexedOperator:
            switch(dexed_op_page_)
            {
                case DexedOpParamPage::Algo: return KnobContext::DexedOpAlgo;
                case DexedOpParamPage::Frequency: return KnobContext::DexedOpFrequency;
                case DexedOpParamPage::Detune: return KnobContext::DexedOpDetune;
                case DexedOpParamPage::OutputLevel: return KnobContext::DexedOpOutputLevel;
                case DexedOpParamPage::EgRate:
                    return dexed_op_egrate_target34_ ? KnobContext::DexedOpEgRate34
                                                     : KnobContext::DexedOpEgRate12;
                case DexedOpParamPage::EgLevel:
                    return dexed_op_eglevel_target34_ ? KnobContext::DexedOpEgLevel34
                                                      : KnobContext::DexedOpEgLevel12;
                case DexedOpParamPage::LevelScalingCurve:
                    return dexed_op_bp_target_ ? KnobContext::DexedOpBreakPoint
                                               : KnobContext::DexedOpLevelScalingCurve;
                case DexedOpParamPage::LevelScalingDepth:
                    return KnobContext::DexedOpLevelScalingDepth;
                case DexedOpParamPage::RateScaling: return KnobContext::DexedOpRateScaling;
                case DexedOpParamPage::Sensitivity: return KnobContext::DexedOpSensitivity;
                case DexedOpParamPage::PitchEgRate:
                    return dexed_pitch_egrate_target34_ ? KnobContext::DexedVoicePitchEgRate34
                                                        : KnobContext::DexedVoicePitchEgRate12;
                case DexedOpParamPage::PitchEgLevel:
                    return dexed_pitch_eglevel_target34_ ? KnobContext::DexedVoicePitchEgLevel34
                                                         : KnobContext::DexedVoicePitchEgLevel12;
                case DexedOpParamPage::Lfo: return KnobContext::DexedVoiceLfo;
                case DexedOpParamPage::LfoDepth: return KnobContext::DexedVoiceLfoDepth;
                case DexedOpParamPage::ModSens: return KnobContext::DexedVoiceModSens;
                case DexedOpParamPage::Transpose: return KnobContext::DexedVoiceTranspose;
                default: return KnobContext::DexedOpAlgo;
            }
        case Screen::Supersaw: return SupersawKnobContext();
        case Screen::Ensemble:
            switch(ensemble_param_page_)
            {
                case EnsembleParamPage::Registration: return KnobContext::EnsembleRegistration;
                case EnsembleParamPage::Perc: return KnobContext::OrganPerc;
                case EnsembleParamPage::Click: return KnobContext::OrganClick;
                case EnsembleParamPage::Env:
                    return ensemble_env_target_sr_ ? KnobContext::EnsembleEnvSR
                                                      : KnobContext::EnsembleEnvAD;
                case EnsembleParamPage::Ensemble: return KnobContext::EnsembleEnsemble;
                case EnsembleParamPage::Lfo: return KnobContext::EnsembleLfo;
                case EnsembleParamPage::LfoDepth: return KnobContext::EnsembleLfoDepth;
                case EnsembleParamPage::Filter: return KnobContext::EnsembleFilter;
                case EnsembleParamPage::Mix:
                    return KnobContext::EnsembleMix;
                case EnsembleParamPage::FX:
                    return ensemble_fx_target_delay_ ? KnobContext::EnsembleFxDelay
                                                        : KnobContext::EnsembleFxReverb;
                case EnsembleParamPage::MIDI:
                    return ensemble_midi_vel_ && ensemble_ && !ensemble_->IsOrgan()
                               ? KnobContext::EnsembleMidiVel
                               : KnobContext::EnsembleMidi;
                case EnsembleParamPage::Preset: return KnobContext::EnsemblePreset;
                case EnsembleParamPage::User: return KnobContext::EnsembleUser;
                default: return KnobContext::EnsembleRegistration;
            }
        case Screen::SampleSynth:
            switch(sample_synth_param_page_)
            {
                case SampleSynthParamPage::Filter: return KnobContext::SampleSynthFilter;
                case SampleSynthParamPage::Env:
                    return sample_synth_adsr_target_sr_ ? KnobContext::SampleSynthEnvSR
                                                            : KnobContext::SampleSynthEnvAD;
                case SampleSynthParamPage::Mix:
                    return sample_synth_mix_target_gain_ ? KnobContext::SampleSynthMixGain
                                                         : KnobContext::SampleSynthMix;
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
                case GranularParamPage::Grain: return KnobContext::GranularGrain;
                case GranularParamPage::Position: return KnobContext::GranularPosition;
                case GranularParamPage::Rhythm: return KnobContext::GranularRhythm;
                case GranularParamPage::Scan: return KnobContext::GranularScan;
                case GranularParamPage::Loop: return KnobContext::GranularLoop;
                case GranularParamPage::Pitch: return KnobContext::GranularPitch;
                case GranularParamPage::ADSR:
                    return granular_adsr_target_sr_ ? KnobContext::GranularEnvSR
                                                       : KnobContext::GranularEnvAD;
                case GranularParamPage::Filter: return KnobContext::GranularFilter;
                case GranularParamPage::Mix:
                    return granular_mix_target_gain_ ? KnobContext::GranularMixGain
                                                            : KnobContext::GranularMix;
                case GranularParamPage::FX:
                    return granular_fx_target_delay_ ? KnobContext::GranularFxDelay
                                                        : KnobContext::GranularFxReverb;
                case GranularParamPage::MIDI: return KnobContext::GranularMidi;
                case GranularParamPage::Capture: return KnobContext::GranularCapture;
                case GranularParamPage::Trim: return KnobContext::GranularTrim;
                case GranularParamPage::Preset: return KnobContext::GranularPreset;
                case GranularParamPage::User: return KnobContext::GranularUser;
                default: return KnobContext::GranularGrain;
            }
        case Screen::Arp:
            return arp_page_ == 0   ? KnobContext::ArpSetup
                   : arp_page_ == 1 ? KnobContext::ArpPattern
                   : arp_page_ == 2 ? KnobContext::ArpFeel
                                    : KnobContext::ArpKey;
        case Screen::Mixer:
            if(mixer_position_ >= MixerSetCount()) // Scope stop
                return KnobContext::MixerNoKnobs;
            if(MixerSetChannel(mixer_position_) == kMixMas)
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
        case KnobContext::HomeScenes:
            k1_pickup_raw_[i] = ((float)scene_cursor_ + 0.5f) / (float)kScenes;
            k2_pickup_raw_[i] = master_volume01_;
            break;
        case KnobContext::GlobalSends:
            k1_pickup_raw_[i] = ((float)sends_ch_ + 0.5f) / (float)kSendChannels;
            k2_pickup_raw_[i] = sends_target_delay_ ? MixerGetDelaySend01(SendsChannel(sends_ch_))
                                                    : MixerGetSend01(SendsChannel(sends_ch_));
            break;
        case KnobContext::GlobalArp:
            k1_pickup_raw_[i] = ((float)EngineColumn(arp_engine_) + 0.5f) / (float)Arpeggiator::kEngines;
            break;
        case KnobContext::ArpSetup:
            if(arp_)
            {
                // K1 is a zone picker (see the Screen::Arp knob code); K2
                // follows whichever setting it picked.
                const Arpeggiator::Settings& s = ArpEdit();
                k2_pickup_raw_[i]
                    = arp_setup_sel_ == 0 ? ((float)s.rate + 0.5f) / (float)Arpeggiator::kNumRates
                      : arp_setup_sel_ == 1 ? (tempo_->GetBpm() - 50.f) / (240.f - 50.f)
                                            : ((float)s.range - 0.5f) / (float)Arpeggiator::kMaxRange;
            }
            break;
        case KnobContext::ArpPattern:
            if(arp_)
            {
                const Arpeggiator::Settings& s = ArpEdit();
                if(s.mode == Arpeggiator::kPhrase)
                {
                    k1_pickup_raw_[i] = ((float)s.phrase + 0.5f) / (float)Arpeggiator::kNumPhrases;
                    k2_pickup_raw_[i] = ((float)(s.octave + 2) + 0.5f) / 5.f;
                }
                else
                {
                    k1_pickup_raw_[i] = ((float)s.steps - 0.5f) / (float)Arpeggiator::kMaxGenSteps;
                    k2_pickup_raw_[i] = ((float)s.jump - 0.5f) / (float)Arpeggiator::kMaxJump;
                }
            }
            break;
        case KnobContext::ArpFeel:
            if(arp_)
            {
                k1_pickup_raw_[i] = ArpEdit().gate01;
                k2_pickup_raw_[i] = ArpEdit().swing01;
            }
            break;
        case KnobContext::ArpKey:
            if(arp_)
            {
                const Arpeggiator::Settings& s = ArpEdit();
                k1_pickup_raw_[i] = ((float)s.key + 0.5f) / 12.f;
                k2_pickup_raw_[i] = ((float)s.scale + 0.5f) / (float)Arpeggiator::kNumScales;
            }
            break;
        case KnobContext::GlobalDisplay:
            k1_pickup_raw_[i] = ((float)saver_option_ + 0.5f) / (float)kSaverOptions;
            break;
        case KnobContext::HomeSceneEdit:
            k1_pickup_raw_[i] = ((float)scene_edit_cell_ + 0.5f) / (float)kSceneCells;
            if(scene_edit_ >= 0)
            {
                const Scene& s = scenes_[scene_edit_];
                const int    c = scene_edit_cell_;
                k2_pickup_raw_[i] = c < Scene::kLayers
                                        ? s.layer_vol01[c]
                                        : ((float)(s.pattern[c - Scene::kLayers] + 1) + 0.5f) / 9.f;
            }
            break;
        case KnobContext::HomeSong:
            k1_pickup_raw_[i] = ((float)song_cursor_ + 0.5f) / (float)(song_.len + 1);
            k2_pickup_raw_[i] = master_volume01_;
            break;
        case KnobContext::HomeSongEdit:
            if(song_cursor_ < song_.len)
            {
                k1_pickup_raw_[i] = ((float)song_.steps[song_cursor_].scene + 0.5f) / (float)kScenes;
                k2_pickup_raw_[i] = ((float)(song_.steps[song_cursor_].repeats - 1) + 0.5f)
                                    / (float)Song::kMaxRepeats;
            }
            break;
        case KnobContext::LayerStatus:
            k1_pickup_raw_[i] = Cur().GetVolume01();
            k2_pickup_raw_[i] = Cur().GetPan01();
            break;
        case KnobContext::LayerFilter:
            k1_pickup_raw_[i] = Cur().GetFilterCutoff01();
            k2_pickup_raw_[i] = Cur().GetFilterResonance01();
            break;
        case KnobContext::LayerDrive:
            k1_pickup_raw_[i] = Cur().GetDrive01();
            k2_pickup_raw_[i] = Cur().GetDriveTrim01();
            break;
        case KnobContext::LayerTremolo:
            k1_pickup_raw_[i] = Cur().GetTremoloDepth01();
            k2_pickup_raw_[i] = Cur().GetTremoloRate01();
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
            break;
        case KnobContext::GlobalDelayReverb:
            k1_pickup_raw_[i] = delay_to_reverb01_;
            break;
        case KnobContext::GlobalDelayTone:
            k1_pickup_raw_[i] = delay_pingpong01_;
            k2_pickup_raw_[i] = delay_tone01_;
            break;
        case KnobContext::GlobalBypassReverb:
            k1_pickup_raw_[i] = bypass_reverb_send01_;
            k2_pickup_raw_[i] = reverb_size01_;
            break;
        case KnobContext::GlobalBypassDelay:
            k1_pickup_raw_[i] = bypass_delay_send01_;
            k2_pickup_raw_[i] = delay_time01_;
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
        case KnobContext::GlobalExport:
            k1_pickup_raw_[i] = ((float)export_mode_ + 0.5f) / 3.f;
            k2_pickup_raw_[i] = ((float)(export_loops_ - 1) + 0.5f) / 8.f;
            break;
        case KnobContext::GlobalGranular: break; // entry point only -- see Screen::Granular instead
        case KnobContext::GlobalLooper: break; // no continuous knobs, Button1 toggle only
        case KnobContext::SeqTrack:
            if(seq_ && HomeSeqTrack() >= 0)
            {
                // Bucket midpoints, so picking up lands on the same setting.
                k1_pickup_raw_[i] = ((float)seq_->GetEngine(HomeSeqTrack()) + 0.5f)
                                    / (float)MidiSequencer::kNumEngines;
                k2_pickup_raw_[i] = ((float)seq_->GetLengthIndex(HomeSeqTrack()) + 0.5f)
                                    / (float)MidiSequencer::kNumLengths;
            }
            break;
        case KnobContext::SeqQuant:
            if(seq_ && HomeSeqTrack() >= 0)
            {
                k1_pickup_raw_[i] = ((float)seq_->GetQuantIndex(HomeSeqTrack()) + 0.5f)
                                    / (float)MidiSequencer::kNumQuant;
                k2_pickup_raw_[i] = (float)seq_->GetSwing(HomeSeqTrack()) / 100.f;
            }
            break;
        case KnobContext::SeqChain:
        case KnobContext::SeqArp:
            k1_pickup_raw_[i] = ((float)seq_patt_cursor_ + 0.5f) / (float)MidiSequencer::kPatterns;
            break;
        case KnobContext::SeqPatt:
            if(seq_ && HomeSeqTrack() >= 0)
            {
                k1_pickup_raw_[i] = ((float)seq_patt_cursor_ + 0.5f) / (float)MidiSequencer::kPatterns;
                k2_pickup_raw_[i] = ((float)(seq_->GetTranspose(HomeSeqTrack(), seq_patt_cursor_)
                                             + MidiSequencer::kMaxTranspose)
                                     + 0.5f)
                                    / (float)(2 * MidiSequencer::kMaxTranspose + 1);
            }
            break;
        case KnobContext::SeqPrint:
            k1_pickup_raw_[i] = ((float)seq_print_dest_ + 0.5f) / (float)(num_layers_ + 1);
            k2_pickup_raw_[i] = seq_print_all_ ? 0.75f : 0.25f;
            break;
        case KnobContext::SeqGrid:
            if(seq_ && HomeSeqTrack() >= 0)
            {
                SeqGridView v     = SeqGridGeometry(HomeSeqTrack());
                // K1: the row position (-1), then each step. K2: left is
                // the top of the screen on both grids.
                k1_pickup_raw_[i] = ((float)grid_col_ + 1.5f) / (float)(v.cells + 1);
                int row_idx       = v.drum ? grid_note_ - v.note_lo : v.note_hi - grid_note_;
                k2_pickup_raw_[i] = ((float)row_idx + 0.5f) / (float)(v.note_hi - v.note_lo + 1);
            }
            break;
        case KnobContext::GlobalMidi:
            // K1 scrolls the rows directly; K2 picks up the selected
            // engine's channel (same bucket-midpoint idiom as DexedMidi).
            k2_pickup_raw_[i] = MidiChannelKnobRaw(*MidiRowChannel(midi_row_));
            break;
        case KnobContext::GlobalMixer:
            k1_pickup_raw_[i] = master_volume01_;
            k2_pickup_raw_[i] = bypass_mix_volume01_;
            break;
        case KnobContext::GlobalSdMgmt: break; // browses a list directly, no pickup used
        case KnobContext::GlobalDexed: break; // entry point only -- see Screen::Dexed instead
        case KnobContext::GlobalEnsemble: break; // entry point only -- see Screen::Ensemble instead
        case KnobContext::DexedAlgo: break; // view-only -- ALGORITHM is set in Advanced
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
                k1_pickup_raw_[i] = dexed_->GetOutputLevel01();
                k2_pickup_raw_[i] = dexed_->GetPan01();
            }
            break;
        case KnobContext::DexedMixGain:
            if(dexed_)
                k1_pickup_raw_[i] = dexed_->GetGain01();
            break;
        case KnobContext::DexedMixA:
        case KnobContext::DexedMixB:
        {
            DexedSynth* e = dexed_engines_[ctx == KnobContext::DexedMixB ? 1 : 0];
            if(e)
            {
                k1_pickup_raw_[i] = e->GetEngineLevel01();
                k2_pickup_raw_[i] = e->GetEnginePan01();
            }
            break;
        }
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
        case KnobContext::DexedSplit:
        {
            // Same bucket-midpoint idiom as DexedMidi below.
            const float span = (float)(kDexedSplitHighNote - kDexedSplitLowNote + 1) - 0.01f;
            k1_pickup_raw_[i]
                = Clampf(((float)(dexed_split_note_ - kDexedSplitLowNote) + 0.5f) / span, 0.f, 1.f);
            k2_pickup_raw_[i]
                = Clampf(((float)(dexed_octave_[dexed_split_oct_b_ ? 1 : 0] + 2) + 0.5f) / 4.99f,
                         0.f, 1.f);
            break;
        }
        case KnobContext::DexedMidi:
            // -1 (Omni) .. 15 (channel 16) -- same bucket-midpoint inverse
            // idiom as GlobalTempo's own Bars knob, just an 18-wide span
            // (17 buckets - 0.01, matching that same epsilon convention)
            // starting at -1 instead of 1.
            k1_pickup_raw_[i] = Clampf(((float)(DexedActiveChannel() + 1) + 0.5f) / 16.99f, 0.f, 1.f);
            // MAX VOICES 1..kMaxVoices, same bucket-midpoint idiom.
            if(dexed_)
                k2_pickup_raw_[i]
                    = Clampf(((float)(dexed_->GetMaxVoices() - 1) + 0.5f)
                                 / ((float)DexedSynth::kMaxVoices - 0.01f),
                             0.f, 1.f);
            break;
        case KnobContext::DexedAdvanced: break; // entry point only -- see Screen::DexedOperator instead
        case KnobContext::DexedPreset: break; // browses a list directly, no pickup used
        case KnobContext::DexedUser: break;   // ditto
        case KnobContext::DexedImport: break; // ditto
        case KnobContext::DexedOpAlgo:
        case KnobContext::DexedOpFrequency:
        case KnobContext::DexedOpDetune:
        case KnobContext::DexedOpOutputLevel:
        case KnobContext::DexedOpEgRate12:
        case KnobContext::DexedOpEgRate34:
        case KnobContext::DexedOpEgLevel12:
        case KnobContext::DexedOpEgLevel34:
        case KnobContext::DexedOpLevelScalingCurve:
        case KnobContext::DexedOpBreakPoint:
        case KnobContext::DexedOpLevelScalingDepth:
        case KnobContext::DexedOpRateScaling:
        case KnobContext::DexedOpSensitivity:
        case KnobContext::DexedVoicePitchEgRate12:
        case KnobContext::DexedVoicePitchEgRate34:
        case KnobContext::DexedVoicePitchEgLevel12:
        case KnobContext::DexedVoicePitchEgLevel34:
        case KnobContext::DexedVoiceLfo:
        case KnobContext::DexedVoiceLfoDepth:
        case KnobContext::DexedVoiceModSens:
        case KnobContext::DexedVoiceTranspose:
            if(dexed_)
            {
                // Same value/max scale ApplyKnobs() rounds back from.
                DexedEditByte e1, e2;
                DexedEditBytesFor(ctx, &e1, &e2);
                if(e1.offset >= 0)
                    k1_pickup_raw_[i] = (float)dexed_->GetPatchByte(e1.offset) / (float)e1.max;
                if(e2.offset >= 0)
                    k2_pickup_raw_[i] = (float)dexed_->GetPatchByte(e2.offset) / (float)e2.max;
            }
            break;
        case KnobContext::OrganPerc:
            if(ensemble_)
            {
                k1_pickup_raw_[i] = ensemble_->GetPercDecay01();
                k2_pickup_raw_[i] = ensemble_->GetPercLevel01();
            }
            break;
        case KnobContext::OrganClick:
            if(ensemble_)
                k1_pickup_raw_[i] = ensemble_->GetClick01();
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
                k2_pickup_raw_[i] = ensemble_->GetGain01();
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
                = Clampf(((float)(*EnsembleActiveChannelPtr() + 1) + 0.5f) / 16.99f, 0.f, 1.f);
            // Voices 1..kMaxVoices, same bucket-midpoint idiom.
            if(ensemble_)
                k2_pickup_raw_[i]
                    = Clampf(((float)(ensemble_->GetMaxVoices() - 1) + 0.5f)
                                 / ((float)EnsembleSynth::kMaxVoices - 0.01f),
                             0.f, 1.f);
            break;
        case KnobContext::SupersawSuperSaw:
        case KnobContext::SupersawOsc2:
        case KnobContext::SupersawBalance:
        case KnobContext::SupersawFilter:
        case KnobContext::SupersawFltEnvAD:
        case KnobContext::SupersawFltEnvSR:
        case KnobContext::SupersawFltMod:
        case KnobContext::SupersawAmpEnvAD:
        case KnobContext::SupersawAmpEnvSR:
        case KnobContext::SupersawLfo:
        case KnobContext::SupersawLfoDepth:
        case KnobContext::SupersawVoice:
        case KnobContext::SupersawChorus:
        case KnobContext::SupersawMix:
        case KnobContext::SupersawFxReverb:
        case KnobContext::SupersawFxDelay:
        case KnobContext::SupersawMidi:
        case KnobContext::SupersawMidiVel: SupersawSyncPickup(ctx, i); break;
        case KnobContext::SupersawList: break; // browses a list directly, no pickup used
        case KnobContext::EnsembleMidiVel:
            if(ensemble_)
                k1_pickup_raw_[i] = ensemble_->GetVelSens01();
            break;
        case KnobContext::EnsembleLfo:
        case KnobContext::EnsembleLfoDepth:
            if(ensemble_)
                LfoSyncPickup(ensemble_->Lfo(), ctx == KnobContext::EnsembleLfoDepth,
                              ensemble_lfo_dest_, i);
            break;
        case KnobContext::EnsemblePreset: break; // browses a list directly, no pickup used
        case KnobContext::EnsembleUser: break;   // ditto
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
        case KnobContext::SampleSynthMixGain:
            if(sample_synth_)
                k1_pickup_raw_[i] = sample_synth_->GetGain01();
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
                = Clampf(((float)(*SampleMidiChannel() + 1) + 0.5f) / 16.99f, 0.f, 1.f);
            if(sample_synth_)
                k2_pickup_raw_[i] = sample_synth_->GetVelSens01();
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
        case KnobContext::GranularGrain:
            if(granular_)
            {
                k1_pickup_raw_[i] = granular_->GetSize01();
                k2_pickup_raw_[i] = granular_->GetDensity01();
            }
            break;
        case KnobContext::GranularPosition:
            if(granular_)
            {
                k1_pickup_raw_[i] = granular_->GetPosition01();
                k2_pickup_raw_[i] = granular_->GetSpray01();
            }
            break;
        case KnobContext::GranularRhythm: break; // Button1/Button2 only
        case KnobContext::GranularScan:
            if(granular_)
            {
                k1_pickup_raw_[i] = granular_->GetScan01();
            }
            break;
        case KnobContext::GranularLoop:
            if(granular_)
            {
                k1_pickup_raw_[i] = granular_->GetLoopStart01();
                k2_pickup_raw_[i] = granular_->GetLoopEnd01();
            }
            break;
        case KnobContext::GranularPitch:
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
                k1_pickup_raw_[i] = granular_->GetOutputLevel01();
                k2_pickup_raw_[i] = granular_->GetPan01();
            }
            break;
        case KnobContext::GranularMixGain:
            if(granular_)
                k1_pickup_raw_[i] = granular_->GetGain01();
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
        case KnobContext::GranularUser: break;   // ditto
        case KnobContext::MixerVolPan:
            k1_pickup_raw_[i] = MixerGetVolume01(MixerSetChannel(mixer_position_));
            k2_pickup_raw_[i] = MixerGetPan01(MixerSetChannel(mixer_position_));
            break;
        case KnobContext::MixerReverb:
            k1_pickup_raw_[i] = MixerGetSend01(MixerSetChannel(mixer_position_));
            k2_pickup_raw_[i] = MixerGetDelaySend01(MixerSetChannel(mixer_position_));
            break;
        case KnobContext::MixerMaster:
            k1_pickup_raw_[i] = master_volume01_;
            k2_pickup_raw_[i] = reverb_size01_;
            break;
        case KnobContext::MixerNoKnobs: break; // Scope stop -- no continuous knobs
        default: break;
    }
}

// Pickup gated on the knob actually being turned since arriving on this
// page/context. KnobPickUp() alone would engage on the first tick if the
// knob merely happened to sit within its epsilon of the setting, and
// then write the knob's own value over it -- arriving on a page could
// change the sound with no knob touched. Reported on Dexed: browse
// presets (both knobs move), scroll on, and a page grabs a knob that
// happens to be close -- the Algorithm page then picked the neighbouring
// algorithm. A knob now does nothing until it has moved kPickupMove from
// where it was on arrival.
bool Ui::PickUpK1(float raw)
{
    if(!k1_moved_)
    {
        if(fabsf(raw - k1_entry_raw_) < kPickupMove)
            return false;
        k1_moved_ = true;
    }
    size_t ci = (size_t)last_knob_context_;
    return KnobPickUp(raw, k1_pickup_raw_[ci], k1_pickup_engaged_[ci]);
}

bool Ui::PickUpK2(float raw)
{
    if(!k2_moved_)
    {
        if(fabsf(raw - k2_entry_raw_) < kPickupMove)
            return false;
        k2_moved_ = true;
    }
    size_t ci = (size_t)last_knob_context_;
    return KnobPickUp(raw, k2_pickup_raw_[ci], k2_pickup_engaged_[ci]);
}

bool Ui::KnobPickUp(float raw, float& stored_raw, bool& engaged)
{
    constexpr float kKnobPickupEpsilon = 0.04f;
    if(!engaged)
    {
        // A setting outside the knob's range (e.g. an imported DX7 value
        // of 127 against a 0..99 knob) would otherwise be unreachable --
        // the knob could never catch it, so it could never be turned down.
        float target = stored_raw < 0.f ? 0.f : (stored_raw > 1.f ? 1.f : stored_raw);
        if(fabsf(raw - target) > kKnobPickupEpsilon)
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
        // Where the knobs are on arrival -- see PickUpK1()/PickUpK2().
        k1_entry_raw_ = k1;
        k2_entry_raw_ = k2;
        k1_moved_     = false;
        k2_moved_     = false;
    }
    size_t ci = (size_t)ctx;

    switch(screen_)
    {
        case Screen::Home:
            if(HomeScenes() && scene_edit_ >= 0)
            {
                if(PickUpK1(k1))
                {
                    const int c = (int)(Clampf(k1, 0.f, 0.999f) * (float)kSceneCells);
                    if(c != scene_edit_cell_)
                    {
                        // K2 now picks up the new cell's value.
                        scene_edit_cell_ = c;
                        const size_t ci  = (size_t)KnobContext::HomeSceneEdit;
                        k2_pickup_engaged_[ci] = false;
                        SyncPickupTargets(KnobContext::HomeSceneEdit);
                    }
                }
                if(PickUpK2(k2))
                    SceneEditValue(k2);
                break;
            }
            if(HomeScenes())
            {
                if(PickUpK1(k1))
                    scene_cursor_ = (int)(Clampf(k1, 0.f, 0.999f) * (float)kScenes);
                if(PickUpK2(k2))
                    SetMasterVolume01(k2);
                break;
            }
            if(HomeSong() && !song_edit_)
            {
                if(PickUpK1(k1))
                {
                    const int c = (int)(Clampf(k1, 0.f, 0.999f) * (float)(song_.len + 1));
                    if(c != song_cursor_)
                        song_k1_ms_ = System::GetNow();
                    song_cursor_ = c;
                }
                if(PickUpK2(k2))
                    SetMasterVolume01(k2);
                break;
            }
            if(HomeSong())
            {
                // Edit: the step's scene and repeats; on "+ add", either
                // knob adds the step.
                const bool k1_moved = PickUpK1(k1), k2_moved = PickUpK2(k2);
                if((k1_moved || k2_moved) && song_cursor_ == song_.len && song_.len < Song::kSteps)
                {
                    song_.steps[song_.len] = song_.len > 0 ? song_.steps[song_.len - 1] : SongStep{};
                    song_.len++;
                }
                if(song_cursor_ < song_.len)
                {
                    SongStep& st = song_.steps[song_cursor_];
                    if(k1_moved)
                        st.scene = (uint8_t)(Clampf(k1, 0.f, 0.999f) * (float)kScenes);
                    if(k2_moved)
                        st.repeats
                            = (uint8_t)(1 + (int)(Clampf(k2, 0.f, 0.999f) * (float)Song::kMaxRepeats));
                }
                break;
            }
            if(PickUpK1(k1))
                SetMasterVolume01(k1);
            if(PickUpK2(k2))
                tempo_->SetMetronomeVolume01(k2);
            break;

        case Screen::SeqTrack:
            if(seq_ && HomeSeqTrack() >= 0 && seq_page_ >= kSeqPageGrid)
                SeqGridKnobs(HomeSeqTrack(), k1, k2);
            else if(seq_page_ == kSeqPageArp)
            {
                // K1 picks the pattern whose arp B1 and a click work on.
                if(PickUpK1(k1))
                    seq_patt_cursor_
                        = (int)(Clampf(k1, 0.f, 0.999f) * (float)MidiSequencer::kPatterns);
            }
            else if(seq_page_ == kSeqPageChain)
            {
                // K1 picks the pattern B2 adds.
                if(PickUpK1(k1))
                    seq_patt_cursor_
                        = (int)(Clampf(k1, 0.f, 0.999f) * (float)MidiSequencer::kPatterns);
            }
            else if(seq_page_ == kSeqPagePatt)
            {
                // K1 just chooses a pattern -- B2 plays it (tap) or copies
                // the playing one into it (hold).
                if(PickUpK1(k1))
                {
                    const int c
                        = (int)(Clampf(k1, 0.f, 0.999f) * (float)MidiSequencer::kPatterns);
                    if(c != seq_patt_cursor_)
                    {
                        // K2 now picks up the new pattern's transpose.
                        seq_patt_cursor_ = c;
                        const size_t ci  = (size_t)KnobContext::SeqPatt;
                        k2_pickup_raw_[ci]
                            = ((float)(seq_->GetTranspose(HomeSeqTrack(), c)
                                       + MidiSequencer::kMaxTranspose)
                               + 0.5f)
                              / (float)(2 * MidiSequencer::kMaxTranspose + 1);
                        k2_pickup_engaged_[ci] = false;
                    }
                }
                // K2: transpose the pattern under the cursor.
                if(PickUpK2(k2))
                    seq_->SetTranspose(HomeSeqTrack(), seq_patt_cursor_,
                                       (int)(Clampf(k2, 0.f, 0.999f)
                                             * (float)(2 * MidiSequencer::kMaxTranspose + 1))
                                           - MidiSequencer::kMaxTranspose);
            }
            else if(seq_page_ == kSeqPagePrint)
            {
                if(PickUpK1(k1) && seq_print_layer_ < 0)
                    seq_print_dest_ = (int)(Clampf(k1, 0.f, 0.999f) * (float)(num_layers_ + 1)); // + Notes
                if(PickUpK2(k2) && seq_print_layer_ < 0)
                    seq_print_all_ = k2 >= 0.5f;
            }
            else if(seq_ && HomeSeqTrack() >= 0 && seq_page_ == kSeqPageQuant)
            {
                const int t = HomeSeqTrack();
                if(PickUpK1(k1))
                    seq_->SetQuantIndex(t, (int)(Clampf(k1, 0.f, 0.999f)
                                                 * (float)MidiSequencer::kNumQuant));
                if(PickUpK2(k2))
                    seq_->SetSwing(t, (int)(Clampf(k2, 0.f, 1.f) * 100.f + 0.5f));
            }
            else if(seq_ && HomeSeqTrack() >= 0)
            {
                const int t = HomeSeqTrack();
                if(PickUpK1(k1))
                    seq_->SetEngine(t, (int)(Clampf(k1, 0.f, 0.999f)
                                             * (float)MidiSequencer::kNumEngines));
                if(PickUpK2(k2))
                {
                    // Lengths that would run past 16 bars at this loop
                    // aren't offered.
                    int li = (int)(Clampf(k2, 0.f, 0.999f) * (float)MidiSequencer::kNumLengths);
                    while(li > 0 && !MidiSequencer::LengthFits(li, tempo_->GetBars()))
                        li--;
                    seq_->SetLengthIndex(t, li);
                }
            }
            break;

        case Screen::Layer:
            switch(layer_page_)
            {
                case LayerPage::Status:
                    if(PickUpK1(k1))
                        Cur().SetVolume01(k1);
                    if(PickUpK2(k2))
                        Cur().SetPan01(k2);
                    break;
                case LayerPage::Filter:
                    if(PickUpK1(k1))
                        Cur().SetFilterCutoff01(k1);
                    if(PickUpK2(k2))
                        Cur().SetFilterResonance01(k2);
                    break;
                case LayerPage::Drive:
                    if(PickUpK1(k1))
                        Cur().SetDrive01(k1);
                    if(PickUpK2(k2))
                        Cur().SetDriveTrim01(k2);
                    break;
                case LayerPage::Tremolo:
                    if(PickUpK1(k1))
                        Cur().SetTremoloDepth01(k1);
                    if(PickUpK2(k2))
                        Cur().SetTremoloRate01(k2);
                    break;
                case LayerPage::Reverb:
                    // Send is per-layer for both effects -- Size/Time/
                    // Feedback are shared Global:FX settings (see the
                    // shared-bus comment on LooperLayer::SetReverbSend01()/
                    // SetDelaySend01()), so this page is just the two Send
                    // knobs, no toggle needed.
                    if(PickUpK1(k1))
                        Cur().SetReverbSend01(k1);
                    if(PickUpK2(k2))
                        Cur().SetDelaySend01(k2);
                    break;
                case LayerPage::Gain:
                    if(PickUpK1(k1))
                        Cur().SetInputGain01(k1);
                    break;
                default: break;
            }
            break;

        case Screen::Global:
            if(global_page_ == GlobalPage::Sends)
            {
                if(PickUpK1(k1))
                {
                    const int c = (int)(Clampf(k1, 0.f, 0.999f) * (float)kSendChannels);
                    if(c != sends_ch_)
                    {
                        // K2 picks up the new channel's send.
                        sends_ch_ = c;
                        k2_pickup_engaged_[(size_t)KnobContext::GlobalSends] = false;
                        SyncPickupTargets(KnobContext::GlobalSends);
                    }
                }
                if(PickUpK2(k2))
                {
                    if(sends_target_delay_)
                        MixerSetDelaySend01(SendsChannel(sends_ch_), k2);
                    else
                        MixerSetSend01(SendsChannel(sends_ch_), k2);
                }
            }
            if(global_page_ == GlobalPage::Arp && PickUpK1(k1))
                arp_engine_ = kEngineShowOrder[(int)(Clampf(k1, 0.f, 0.999f) * (float)Arpeggiator::kEngines)];
            if(global_page_ == GlobalPage::Display && PickUpK1(k1))
                saver_option_ = (int)(Clampf(k1, 0.f, 0.999f) * (float)kSaverOptions);
            if(global_page_ == GlobalPage::Export && !live_open_)
            {
                // K1: Fast (offline, the layers) or Live (the master as it
                // plays, MIDI tracks included); K2: how many loops Live takes.
                if(PickUpK1(k1))
                    export_mode_ = (int)(Clampf(k1, 0.f, 0.999f) * 3.f);
                if(PickUpK2(k2))
                    export_loops_ = 1 + (int)(Clampf(k2, 0.f, 0.999f) * 8.f);
            }
            if(global_page_ == GlobalPage::Tempo && !tempo_->IsLocked())
            {
                if(PickUpK1(k1))
                    tempo_->SetBpm(50.f + k1 * (240.f - 50.f));
                if(PickUpK2(k2))
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
                if(PickUpK1(k1))
                    master_filter_cutoff01_ = Clampf(k1, 0.f, 1.f);
                if(PickUpK2(k2))
                    master_filter_res01_ = Clampf(k2, 0.f, 1.f);
            }
            else if(global_page_ == GlobalPage::Reverb)
            {
                if(PickUpK1(k1))
                    reverb_size01_ = Clampf(k1, 0.f, 1.f);
            }
            else if(global_page_ == GlobalPage::Delay)
            {
                if(PickUpK1(k1))
                    delay_time01_ = Clampf(k1, 0.f, 1.f);
                if(PickUpK2(k2))
                    delay_feedback01_ = Clampf(k2, 0.f, 1.f);
            }
            else if(global_page_ == GlobalPage::DelayReverb)
            {
                if(PickUpK1(k1))
                    delay_to_reverb01_ = Clampf(k1, 0.f, 1.f);
            }
            else if(global_page_ == GlobalPage::DelayTone)
            {
                if(PickUpK1(k1))
                    delay_pingpong01_ = Clampf(k1, 0.f, 1.f);
                if(PickUpK2(k2))
                    delay_tone01_ = Clampf(k2, 0.f, 1.f);
            }
            else if(global_page_ == GlobalPage::Bypass)
            {
                // Same shape as every engine's own FX page: K1 = this
                // source's Send, K2 = the shared bus's Size/Time.
                if(!bypass_fx_target_delay_)
                {
                    if(PickUpK1(k1))
                        bypass_reverb_send01_ = Clampf(k1, 0.f, 1.f);
                    if(PickUpK2(k2))
                        reverb_size01_ = Clampf(k2, 0.f, 1.f);
                }
                else
                {
                    if(PickUpK1(k1))
                        bypass_delay_send01_ = Clampf(k1, 0.f, 1.f);
                    if(PickUpK2(k2))
                        delay_time01_ = Clampf(k2, 0.f, 1.f);
                }
            }
            else if(global_page_ == GlobalPage::ReverbTone)
            {
                if(!global_reverb_tone_target_b_)
                {
                    if(PickUpK1(k1))
                        reverb_predelay01_ = Clampf(k1, 0.f, 1.f);
                    if(PickUpK2(k2))
                        reverb_prefilter01_ = Clampf(k2, 0.f, 1.f);
                }
                else
                {
                    if(PickUpK1(k1))
                        reverb_decay_diffusion01_ = Clampf(k1, 0.f, 1.f);
                    if(PickUpK2(k2))
                        reverb_damping01_ = Clampf(k2, 0.f, 1.f);
                }
            }
            else if(global_page_ == GlobalPage::ReverbDiffusion)
            {
                if(PickUpK1(k1))
                    reverb_input_diffusion1_01_ = Clampf(k1, 0.f, 1.f);
                if(PickUpK2(k2))
                    reverb_input_diffusion2_01_ = Clampf(k2, 0.f, 1.f);
            }
            else if(global_page_ == GlobalPage::Speed)
            {
                if(PickUpK1(k1))
                    SetProjectSpeed01(k1);
            }
            else if(global_page_ == GlobalPage::File)
            {
                if(save_load_mode_ == SaveLoadMode::ChoosingSave)
                {
                    // Overwrite is only a real option once something's
                    // actually loaded -- otherwise force Save New.
                    save_as_new_ = loaded_slot_ < 0 ? true : k1 >= 0.5f;
                }
                else if(save_load_mode_ == SaveLoadMode::BrowsingLoad)
                {
                    // K2 scrolls "New" + the saved performances.
                    int total = 1 + file_slot_count_;
                    int idx   = (int)(Clampf(k2, 0.f, 1.f) * total);
                    if(idx >= total)
                        idx = total - 1;
                    if(idx != file_cursor_)
                        file_status_[0] = '\0';
                    file_cursor_ = idx;
                }
            }
            else if(global_page_ == GlobalPage::Midi)
            {
                // K1: the engine row, read directly like Snap's list --
                // but only once it has actually moved, so arriving on
                // the page doesn't jump the selection.
                if(k1_moved_ || fabsf(k1 - k1_entry_raw_) >= kPickupMove)
                {
                    k1_moved_ = true;
                    int row   = kEngineShowOrder[(int)(Clampf(k1, 0.f, 1.f) * ((float)kMidiRows - 0.01f))];
                    if(row != midi_row_)
                    {
                        midi_row_ = row;
                        // K2 now sets a different engine: pick up from
                        // that one's channel, not where K2 happens to be.
                        size_t ci             = (size_t)KnobContext::GlobalMidi;
                        k2_pickup_engaged_[ci] = false;
                        k2_pickup_raw_[ci]     = MidiChannelKnobRaw(*MidiRowChannel(midi_row_));
                    }
                }
                if(PickUpK2(k2))
                {
                    int ch = -1 + (int)(Clampf(k2, 0.f, 1.f) * 16.99f);
                    // DXD-B while locked: setting its own channel unlocks it.
                    if(midi_row_ == 2 && dexed_midi_lock_ && ch != dexed_midi_channel_)
                        dexed_midi_lock_ = false;
                    *MidiRowChannel(midi_row_) = ch;
                }
            }
            else if(global_page_ == GlobalPage::Snap)
            {
                if(save_load_mode_ == SaveLoadMode::ChoosingSave)
                    save_as_new_ = snap_loaded_slot_ < 0 ? true : k1 >= 0.5f;
                else if(save_load_mode_ == SaveLoadMode::BrowsingLoad && snap_slot_count_ > 0)
                {
                    int idx = (int)(Clampf(k2, 0.f, 1.f) * snap_slot_count_);
                    if(idx >= snap_slot_count_)
                        idx = snap_slot_count_ - 1;
                    if(idx != snap_cursor_)
                        snap_status_[0] = '\0';
                    snap_cursor_ = idx;
                }
            }
            else if(global_page_ == GlobalPage::MixTracks || global_page_ == GlobalPage::MixSynths)
            {
                // The two volumes you reach for live: Master, and Bypass
                // on its own -- Bypass also goes through Master, so with
                // Master up a mic monitored through Bypass can feed back
                // unless Bypass itself is turned down. Same values as
                // Screen::Mixer's Master and Bypass channels.
                if(PickUpK1(k1))
                    SetMasterVolume01(k1);
                if(PickUpK2(k2))
                    SetBypassMixVolume01(k2);
            }
            else if(global_page_ == GlobalPage::SdMgmt)
            {
                // Shared browse convention: K1 scrolls the box grid and
                // every folder level, K2 scrolls an item list (K1 left
                // alone, so Back lands on the same folder).
                if(!sd_mgmt_in_folder_)
                {
                    int n   = (int)SdMgmtFolder::kCount;
                    int idx = (int)(Clampf(k1, 0.f, 1.f) * n);
                    if(idx >= n)
                        idx = n - 1;
                    sd_mgmt_folder_ = (SdMgmtFolder)idx;
                }
                else
                {
                    bool at_list = SdMgmtAtFolderList();
                    int  count   = SdMgmtListCount();
                    if(count > 0)
                    {
                        int idx = (int)(Clampf(at_list ? k1 : k2, 0.f, 1.f) * count);
                        if(idx >= count)
                            idx = count - 1;
                        int& cur = at_list ? sd_mgmt_list_cursor_ : sd_mgmt_cursor_;
                        if(idx != cur)
                            sd_mgmt_status_[0] = '\0';
                        cur = idx;
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
                    if(PickUpK1(k1))
                        granular_->SetSize01(k1);
                    if(PickUpK2(k2))
                        granular_->SetDensity01(k2);
                    break;
                case GranularParamPage::Position:
                    if(PickUpK1(k1))
                        granular_->SetPosition01(k1);
                    if(PickUpK2(k2))
                        granular_->SetSpray01(k2);
                    break;
                case GranularParamPage::Rhythm: break; // Button1/Button2 only
                case GranularParamPage::Scan:
                    if(PickUpK1(k1))
                        granular_->SetScan01(k1);
                    break;
                case GranularParamPage::Loop:
                    if(PickUpK1(k1))
                        granular_->SetLoopStart01(k1);
                    if(PickUpK2(k2))
                        granular_->SetLoopEnd01(k2);
                    break;
                case GranularParamPage::Pitch:
                    if(PickUpK1(k1))
                        granular_->SetGrainTuneSemitones01(k1);
                    break;
                case GranularParamPage::ADSR:
                    if(!granular_adsr_target_sr_)
                    {
                        if(PickUpK1(k1))
                            granular_->SetAttack01(k1);
                        if(PickUpK2(k2))
                            granular_->SetDecay01(k2);
                    }
                    else
                    {
                        if(PickUpK1(k1))
                            granular_->SetSustain01(k1);
                        if(PickUpK2(k2))
                            granular_->SetRelease01(k2);
                    }
                    break;
                case GranularParamPage::Filter:
                    if(PickUpK1(k1))
                        granular_->SetFilterCutoff01(k1);
                    if(PickUpK2(k2))
                        granular_->SetFilterResonance01(k2);
                    break;
                case GranularParamPage::Mix:
                    if(!granular_mix_target_gain_)
                    {
                        if(PickUpK1(k1))
                            granular_->SetOutputLevel01(k1);
                        if(PickUpK2(k2))
                            granular_->SetPan01(k2);
                    }
                    else if(PickUpK1(k1))
                        granular_->SetGain01(k1);
                    break;
                case GranularParamPage::FX:
                    if(!granular_fx_target_delay_)
                    {
                        if(PickUpK1(k1))
                            granular_->SetReverbSend01(k1);
                        if(PickUpK2(k2))
                            reverb_size01_ = Clampf(k2, 0.f, 1.f);
                    }
                    else
                    {
                        if(PickUpK1(k1))
                            granular_->SetDelaySend01(k1);
                        if(PickUpK2(k2))
                            delay_time01_ = Clampf(k2, 0.f, 1.f);
                    }
                    break;
                case GranularParamPage::MIDI:
                    if(PickUpK1(k1))
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
                    if(PickUpK1(k1))
                    {
                        granular_trim_start01_ = k1;
                        changed                = true;
                    }
                    if(PickUpK2(k2))
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
                    // K1 scrolls the folder list; inside a folder K2
                    // scrolls the sounds and K1 is left alone, so it still
                    // points at this folder on Back.
                    if(save_load_mode_ != SaveLoadMode::BrowsingLoad)
                        break;
                    if(granular_lib_folder_ < 0)
                    {
                        int n = 1 + granular_captured_bank_count_; // Starter + Captured
                        int idx = (int)(Clampf(k1, 0.f, 1.f) * n);
                        if(idx >= n)
                            idx = n - 1;
                        granular_lib_folder_cursor_ = idx;
                        break;
                    }
                    int total = granular_lib_count_;
                    if(total > 0)
                    {
                        int idx = (int)(Clampf(k2, 0.f, 1.f) * total);
                        if(idx >= total)
                            idx = total - 1;
                        if(idx != granular_lib_cursor_)
                            granular_preset_status_[0] = '\0';
                        granular_lib_cursor_ = idx;
                    }
                    break;
                }
                case GranularParamPage::User:
                {
                    if(save_load_mode_ == SaveLoadMode::ChoosingSave)
                    {
                        // Overwrite only makes sense once a user save is loaded.
                        save_as_new_ = granular_loaded_preset_slot_ <= 0 ? true : k1 >= 0.5f;
                    }
                    else if(save_load_mode_ == SaveLoadMode::BrowsingLoad)
                    {
                        int total = 1 + granular_preset_user_slot_count_; // "New" + saves
                        int idx   = (int)(Clampf(k2, 0.f, 1.f) * total);
                        if(idx >= total)
                            idx = total - 1;
                        if(idx != granular_user_cursor_)
                            granular_preset_status_[0] = '\0';
                        granular_user_cursor_ = idx;
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
                case DexedParamPage::Algo: break; // view-only -- set in Advanced
                case DexedParamPage::Feedback:
                    if(PickUpK1(k1))
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
                        if(PickUpK1(k1))
                        {
                            int speed = (int)(Clampf(k1, 0.f, 1.f) * 99.f + 0.5f);
                            speed     = speed > 99 ? 99 : speed;
                            if((uint8_t)speed != dexed_->GetPatchByte(137))
                                dexed_->SetPatchByte(137, (uint8_t)speed);
                        }
                        if(PickUpK2(k2))
                        {
                            int depth = (int)(Clampf(k2, 0.f, 1.f) * 99.f + 0.5f);
                            depth     = depth > 99 ? 99 : depth;
                            if((uint8_t)depth != dexed_->GetPatchByte(139))
                                dexed_->SetPatchByte(139, (uint8_t)depth);
                        }
                    }
                    else if(PickUpK1(k1))
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
                    if(PickUpK1(k1)
                       && fabsf(k1 - dexed_->GetBrightness01()) > 0.002f)
                        dexed_->SetBrightness01(k1);
                    break;
                case DexedParamPage::EnvSpeed:
                    if(PickUpK1(k1)
                       && fabsf(k1 - dexed_->GetEnvSpeed01()) > 0.002f)
                        dexed_->SetEnvSpeed01(k1);
                    break;
                case DexedParamPage::Filter:
                    if(PickUpK1(k1))
                        dexed_->SetFilterCutoff01(k1);
                    if(PickUpK2(k2))
                        dexed_->SetFilterResonance01(k2);
                    break;
                case DexedParamPage::Mix:
                    if(dexed_mix_target_gain_)
                    {
                        if(PickUpK1(k1))
                            dexed_->SetGain01(k1);
                        break;
                    }
                    if(dexed_mix_sel_ < 2)
                    {
                        DexedSynth* e = dexed_engines_[dexed_mix_sel_];
                        if(PickUpK1(k1))
                            e->SetEngineLevel01(k1);
                        if(PickUpK2(k2))
                            e->SetEnginePan01(k2);
                        break;
                    }
                    if(PickUpK1(k1))
                        dexed_->SetOutputLevel01(k1);
                    if(PickUpK2(k2))
                        dexed_->SetPan01(k2);
                    break;
                case DexedParamPage::FX:
                    if(!dexed_fx_target_delay_)
                    {
                        if(PickUpK1(k1))
                            dexed_->SetReverbSend01(k1);
                        if(PickUpK2(k2))
                            reverb_size01_ = Clampf(k2, 0.f, 1.f);
                    }
                    else
                    {
                        if(PickUpK1(k1))
                            dexed_->SetDelaySend01(k1);
                        if(PickUpK2(k2))
                            delay_time01_ = Clampf(k2, 0.f, 1.f);
                    }
                    break;
                case DexedParamPage::Fold:
                    // K1 = Fold Amount, K2 = output trim -- no
                    // sub-mode, unlike FX above.
                    if(PickUpK1(k1))
                        dexed_->SetFoldAmount01(k1);
                    if(PickUpK2(k2))
                        dexed_->SetFoldTrim01(k2);
                    break;
                case DexedParamPage::MIDI:
                    if(PickUpK1(k1))
                        *DexedActiveChannelPtr() = -1 + (int)(k1 * 16.99f);
                    if(PickUpK2(k2))
                    {
                        // Stops at whatever the other engine leaves free
                        // while both are on (see DexedVoiceRoom()).
                        int n    = 1 + (int)(k2 * ((float)DexedSynth::kMaxVoices - 0.01f));
                        int room = DexedVoiceRoom(dexed_active_);
                        dexed_->SetMaxVoices(n < room ? n : room);
                    }
                    break;
                case DexedParamPage::Split:
                    if(PickUpK1(k1))
                        dexed_split_note_
                            = kDexedSplitLowNote
                              + (int)(k1 * ((float)(kDexedSplitHighNote - kDexedSplitLowNote + 1) - 0.01f));
                    if(PickUpK2(k2))
                        dexed_octave_[dexed_split_oct_b_ ? 1 : 0] = -2 + (int)(k2 * 4.99f);
                    break;
                case DexedParamPage::Advanced: break; // no knobs, encoder click drills in
                case DexedParamPage::Preset:
                {
                    // K1 scrolls the group list and a group's folders;
                    // inside a folder K2 scrolls the presets and K1 is
                    // left alone, so it still points at this folder on Back.
                    if(save_load_mode_ != SaveLoadMode::BrowsingLoad)
                        break;
                    if(!dexed_files_group_open_)
                    {
                        // Only the group list's own labels are drawn here --
                        // no SD scan per group swept past (that was laggy);
                        // the real refresh happens on Open.
                        int total = (int)DexedFilesGroup::kCount;
                        int idx   = (int)(Clampf(k1, 0.f, 1.f) * total);
                        if(idx >= total)
                            idx = total - 1;
                        dexed_files_group_ = (DexedFilesGroup)idx;
                        break;
                    }
                    if(!dexed_preset_folder_open_)
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
                    int total;
                    if(dexed_files_group_ == DexedFilesGroup::Imports)
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
                    int idx = (int)(Clampf(k2, 0.f, 1.f) * total);
                    if(idx >= total)
                        idx = total - 1;
                    if(idx != dexed_preset_cursor_)
                        dexed_preset_status_[0] = '\0';
                    dexed_preset_cursor_ = idx;
                    break;
                }
                case DexedParamPage::User:
                {
                    if(save_load_mode_ == SaveLoadMode::ChoosingSave)
                    {
                        // Overwrite only makes sense once a user save is loaded.
                        save_as_new_
                            = dexed_loaded_preset_slot_ <= DexedSynth::GetNumFactoryPresets()
                                  ? true
                                  : k1 >= 0.5f;
                    }
                    else if(save_load_mode_ == SaveLoadMode::BrowsingLoad)
                    {
                        int total = 1 + dexed_preset_user_slot_count_; // "New" + saves
                        int idx   = (int)(Clampf(k2, 0.f, 1.f) * total);
                        if(idx >= total)
                            idx = total - 1;
                        if(idx != dexed_user_cursor_)
                            dexed_preset_status_[0] = '\0';
                        dexed_user_cursor_ = idx;
                    }
                    break;
                }
                case DexedParamPage::Import:
                {
                    // K2 scrolls the .syx files (a file list, same as any
                    // open folder).
                    int total = dexed_import_file_count_;
                    if(total > 0)
                    {
                        int idx = (int)(Clampf(k2, 0.f, 1.f) * total);
                        if(idx >= total)
                            idx = total - 1;
                        if(idx != dexed_import_cursor_)
                            dexed_preset_status_[0] = '\0';
                        dexed_import_cursor_ = idx;
                    }
                    break;
                }
                default: break;
            }
            break;

        case Screen::Supersaw: SupersawApplyKnobs(k1, k2, ci); break;

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
                    const int nbars = ensemble_->NumDrawbars(); // 7, or 9 on the Organ
                    int       idx   = (int)(Clampf(k1, 0.f, 1.f) * nbars);
                    if(idx >= nbars)
                        idx = nbars - 1;
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
                    if(ensemble_ && PickUpK2(k2))
                        ensemble_->SetDrawbarLevel(ensemble_drawbar_index_, k2);
                    break;
                }
                case EnsembleParamPage::Env:
                    if(!ensemble_env_target_sr_)
                    {
                        if(PickUpK1(k1))
                            ensemble_->SetAttack01(k1);
                        if(PickUpK2(k2))
                            ensemble_->SetDecay01(k2);
                    }
                    else
                    {
                        if(PickUpK1(k1))
                            ensemble_->SetSustain01(k1);
                        if(PickUpK2(k2))
                            ensemble_->SetRelease01(k2);
                    }
                    break;
                case EnsembleParamPage::Lfo:
                case EnsembleParamPage::LfoDepth:
                    LfoApplyKnobs(ensemble_->Lfo(), ensemble_param_page_ == EnsembleParamPage::LfoDepth,
                                  &ensemble_lfo_dest_, k1, k2, ci);
                    break;
                case EnsembleParamPage::Ensemble:
                    if(PickUpK1(k1))
                        ensemble_->SetChorusDepth01(k1);
                    if(PickUpK2(k2))
                        ensemble_->SetChorusRate01(k2);
                    break;
                case EnsembleParamPage::Filter:
                    if(PickUpK1(k1))
                        ensemble_->SetFilterCutoff01(k1);
                    if(PickUpK2(k2))
                        ensemble_->SetFilterResonance01(k2);
                    break;
                case EnsembleParamPage::Mix: // Pan is on the Mixer
                    if(PickUpK1(k1))
                        ensemble_->SetOutputLevel01(k1);
                    if(PickUpK2(k2))
                        ensemble_->SetGain01(k2);
                    break;
                case EnsembleParamPage::FX:
                    if(!ensemble_fx_target_delay_)
                    {
                        if(PickUpK1(k1))
                            ensemble_->SetReverbSend01(k1);
                        if(PickUpK2(k2))
                            reverb_size01_ = Clampf(k2, 0.f, 1.f);
                    }
                    else
                    {
                        if(PickUpK1(k1))
                            ensemble_->SetDelaySend01(k1);
                        if(PickUpK2(k2))
                            delay_time01_ = Clampf(k2, 0.f, 1.f);
                    }
                    break;
                case EnsembleParamPage::Perc:
                    if(PickUpK1(k1))
                        ensemble_->SetPercDecay01(k1);
                    if(PickUpK2(k2))
                        ensemble_->SetPercLevel01(k2);
                    break;
                case EnsembleParamPage::Click:
                    if(PickUpK1(k1))
                        ensemble_->SetClick01(k1);
                    break;
                case EnsembleParamPage::MIDI:
                    if(ensemble_midi_vel_ && !ensemble_->IsOrgan())
                    {
                        if(PickUpK1(k1))
                            ensemble_->SetVelSens01(k1);
                        break;
                    }
                    if(PickUpK1(k1))
                        *EnsembleActiveChannelPtr() = -1 + (int)(k1 * 16.99f);
                    if(PickUpK2(k2))
                        ensemble_->SetMaxVoices(
                            1 + (int)(k2 * ((float)EnsembleSynth::kMaxVoices - 0.01f)));
                    break;
                case EnsembleParamPage::Preset:
                {
                    // K2 scrolls the factory sounds (shared browse convention).
                    int total = ensemble_->GetNumFactoryPresets();
                    if(save_load_mode_ == SaveLoadMode::BrowsingLoad && total > 0)
                    {
                        int idx = (int)(Clampf(k2, 0.f, 1.f) * total);
                        if(idx >= total)
                            idx = total - 1;
                        if(idx != ensemble_preset_cursor_)
                            ensemble_preset_status_[0] = '\0';
                        ensemble_preset_cursor_ = idx;
                    }
                    break;
                }
                case EnsembleParamPage::User:
                {
                    if(save_load_mode_ == SaveLoadMode::ChoosingSave)
                    {
                        // Overwrite only makes sense once a user save is loaded.
                        save_as_new_ = !EnsembleIsUserSlot(ensemble_loaded_preset_slot_)
                                           ? true
                                           : k1 >= 0.5f;
                    }
                    else if(save_load_mode_ == SaveLoadMode::BrowsingLoad)
                    {
                        int total = 1 + ensemble_preset_user_slot_count_; // "New" + saves
                        int idx   = (int)(Clampf(k2, 0.f, 1.f) * total);
                        if(idx >= total)
                            idx = total - 1;
                        if(idx != ensemble_user_cursor_)
                            ensemble_preset_status_[0] = '\0';
                        ensemble_user_cursor_ = idx;
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
                    if(PickUpK1(k1))
                        sample_synth_->SetFilterCutoff01(k1);
                    if(PickUpK2(k2))
                        sample_synth_->SetFilterResonance01(k2);
                    break;
                case SampleSynthParamPage::Env:
                    if(!sample_synth_adsr_target_sr_)
                    {
                        if(PickUpK1(k1))
                            sample_synth_->SetAttack01(k1);
                        if(PickUpK2(k2))
                            sample_synth_->SetDecay01(k2);
                    }
                    else
                    {
                        if(PickUpK1(k1))
                            sample_synth_->SetSustain01(k1);
                        if(PickUpK2(k2))
                            sample_synth_->SetRelease01(k2);
                    }
                    break;
                case SampleSynthParamPage::Mix:
                    if(sample_synth_mix_target_gain_)
                    {
                        if(PickUpK1(k1))
                            sample_synth_->SetGain01(k1);
                        break;
                    }
                    if(PickUpK1(k1))
                        sample_synth_->SetOutputLevel01(k1);
                    if(PickUpK2(k2))
                        sample_synth_->SetPan01(k2);
                    break;
                case SampleSynthParamPage::Setup:
                    if(PickUpK1(k1))
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
                    if(PickUpK2(k2))
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
                        if(PickUpK1(k1))
                            sample_synth_->SetReverbSend01(k1);
                        if(PickUpK2(k2))
                            reverb_size01_ = Clampf(k2, 0.f, 1.f);
                    }
                    else
                    {
                        if(PickUpK1(k1))
                            sample_synth_->SetDelaySend01(k1);
                        if(PickUpK2(k2))
                            delay_time01_ = Clampf(k2, 0.f, 1.f);
                    }
                    break;
                case SampleSynthParamPage::MIDI:
                    if(PickUpK1(k1))
                        *SampleMidiChannel() = -1 + (int)(k1 * 16.99f);
                    if(PickUpK2(k2))
                        sample_synth_->SetVelSens01(k2);
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
                    if(PickUpK1(k1))
                    {
                        sample_synth_trim_start01_ = k1;
                        changed                     = true;
                    }
                    if(PickUpK2(k2))
                    {
                        sample_synth_trim_end01_ = k2;
                        changed                   = true;
                    }
                    if(changed)
                        ApplySampleSynthTrim();
                    break;
                }
                case SampleSynthParamPage::Tune:
                    if(PickUpK1(k1))
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
                    if(PickUpK2(k2))
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
                case SampleSynthParamPage::Import:
                {
                    // K1 picks Kits/Breaks first on SMPDR, then the
                    // destination (absolute, like every list).
                    if(!SampleSynthImportReady())
                    {
                        sample_synth_import_drum_breaks_ = Clampf(k1, 0.f, 1.f) >= 0.5f;
                        break;
                    }
                    int n = sample_synth_import_dest_count_;
                    if(n > 0)
                    {
                        int idx = (int)(Clampf(k1, 0.f, 1.f) * n);
                        sample_synth_import_dest_cursor_ = idx >= n ? n - 1 : idx;
                    }
                    break;
                }
                case SampleSynthParamPage::Preset:
                {
                    // Direct "browse a list" idiom, no pickup -- same
                    // shape as Dexed/Grains' own Import file lists.
                    // At the bank level (sample_synth_sound_category_
                    // < 0) this same knob browses the bank list instead
                    // -- see sample_synth_sound_category_'s own comment.
                    if(AtSampleSynthVoiceTypeSelect())
                    {
                        int nt  = NumFaceBankTypes();
                        int idx = (int)(Clampf(k1, 0.f, 1.f) * nt);
                        sample_synth_voice_type_cursor_ = idx >= nt ? nt - 1 : idx;
                        break;
                    }
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
                    if(save_load_mode_ == SaveLoadMode::BrowsingLoad && !is_voice)
                    {
                        // SMPDR: K1 scrolls Kits/Breaks, K2 the saves in an
                        // open folder (K1 left alone, so Back lands on it).
                        if(sample_synth_drum_user_folder_ < 0)
                            sample_synth_drum_user_folder_cursor_ = Clampf(k1, 0.f, 1.f) >= 0.5f ? 1 : 0;
                        else
                        {
                            int n = sample_synth_drum_user_folder_count_[sample_synth_drum_user_folder_];
                            if(n > 0)
                            {
                                int k = (int)(Clampf(k2, 0.f, 1.f) * n);
                                if(k >= n)
                                    k = n - 1;
                                int idx = DrumUserGlobalIndex(sample_synth_drum_user_folder_, k);
                                if(idx != cursor)
                                    status[0] = '\0';
                                cursor = idx;
                            }
                        }
                    }
                    else if(count > 0 && save_load_mode_ == SaveLoadMode::BrowsingLoad)
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
            // Every Advanced page is one or two plain patch bytes -- see
            // DexedEditBytesFor() for the per-page mapping.
            DexedEditByte e1, e2;
            DexedEditBytesFor(CurrentKnobContext(), &e1, &e2);
            if(e1.offset >= 0 && PickUpK1(k1))
            {
                int v = (int)(Clampf(k1, 0.f, 1.f) * (float)e1.max + 0.5f);
                v     = v > e1.max ? e1.max : v;
                if((uint8_t)v != dexed_->GetPatchByte(e1.offset))
                    dexed_->SetPatchByte(e1.offset, (uint8_t)v);
            }
            if(e2.offset >= 0 && PickUpK2(k2))
            {
                int v = (int)(Clampf(k2, 0.f, 1.f) * (float)e2.max + 0.5f);
                v     = v > e2.max ? e2.max : v;
                if((uint8_t)v != dexed_->GetPatchByte(e2.offset))
                    dexed_->SetPatchByte(e2.offset, (uint8_t)v);
            }
            break;
        }

        case Screen::Arp:
            if(arp_)
            {
                Arpeggiator::Settings& s = ArpEdit();
                // (Setup's K1 is a zone picker with its own pickup.)
                const bool p1 = arp_page_ != 0 && PickUpK1(k1), p2 = PickUpK2(k2);
                if(arp_page_ == 0)
                {
                    // K1's travel is Rate, Tempo, Range twice over, so a
                    // setting is never far. It takes over (like a pickup)
                    // once it's in a zone for the setting already picked;
                    // after that its zone picks, and K2 picks up the new
                    // setting's value.
                    (void)p1;
                    const size_t ci   = (size_t)KnobContext::ArpSetup;
                    const int    zone = (int)(Clampf(k1, 0.f, 0.999f) * 6.f) % 3;
                    if(!k1_pickup_engaged_[ci])
                        k1_pickup_engaged_[ci] = zone == arp_setup_sel_;
                    else if(zone != arp_setup_sel_)
                    {
                        arp_setup_sel_         = zone;
                        k2_pickup_engaged_[ci] = false;
                        SyncPickupTargets(KnobContext::ArpSetup);
                    }
                    else if(p2)
                    {
                        if(arp_setup_sel_ == 0)
                            s.rate = (uint8_t)(Clampf(k2, 0.f, 0.999f) * (float)Arpeggiator::kNumRates);
                        else if(arp_setup_sel_ == 1)
                        {
                            if(!tempo_->IsLocked()) // a recording fixes it
                                tempo_->SetBpm(50.f + Clampf(k2, 0.f, 1.f) * (240.f - 50.f));
                        }
                        else
                            s.range = (uint8_t)(1 + Clampf(k2, 0.f, 0.999f) * (float)Arpeggiator::kMaxRange);
                    }
                }
                else if(arp_page_ == 1 && s.mode == Arpeggiator::kPhrase)
                {
                    if(p1)
                        s.phrase = (uint8_t)(Clampf(k1, 0.f, 0.999f) * (float)Arpeggiator::kNumPhrases);
                    if(p2) // the phrase up or down an octave or two
                        s.octave = (int8_t)((int)(Clampf(k2, 0.f, 0.999f) * 5.f) - 2);
                }
                else if(arp_page_ == 1)
                {
                    if(p1)
                        s.steps = (uint8_t)(1 + Clampf(k1, 0.f, 0.999f) * (float)Arpeggiator::kMaxGenSteps);
                    if(p2)
                        s.jump = (uint8_t)(1 + Clampf(k2, 0.f, 0.999f) * (float)Arpeggiator::kMaxJump);
                }
                else if(arp_page_ == 2)
                {
                    if(p1)
                        s.gate01 = 0.05f + 0.95f * Clampf(k1, 0.f, 1.f);
                    if(p2)
                        s.swing01 = Clampf(k2, 0.f, 1.f);
                }
                else
                {
                    if(p1)
                        s.key = (uint8_t)(Clampf(k1, 0.f, 0.999f) * 12.f);
                    if(p2)
                        s.scale = (uint8_t)(Clampf(k2, 0.f, 0.999f) * (float)Arpeggiator::kNumScales);
                }
            }
            break;

        case Screen::Mixer:
        {
            if(mixer_position_ >= MixerSetCount()) // Scope stop -- no continuous knobs
                break;
            const int ch = MixerSetChannel(mixer_position_);
            if(ch == kMixMas) // Master: Volume + Reverb Size, no toggle
            {
                if(PickUpK1(k1))
                {
                    master_volume01_ = Clampf(k1, 0.f, 1.f);
                    master_volume_   = powf(master_volume01_, 2.5f) * 1.43f;
                    if(master_volume_ < 0.f)
                        master_volume_ = 0.f;
                }
                if(PickUpK2(k2))
                    reverb_size01_ = Clampf(k2, 0.f, 1.f);
            }
            else if(!mixer_target_reverb_)
            {
                if(PickUpK1(k1))
                    MixerSetVolume01(ch, k1);
                if(PickUpK2(k2))
                    MixerSetPan01(ch, k2);
            }
            else
            {
                if(PickUpK1(k1))
                    MixerSetSend01(ch, k1);
                if(PickUpK2(k2))
                    MixerSetDelaySend01(ch, k2);
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
    bool any_playing = false, any_paused = false, any_count_in = false;
    for(int i = 0; i < num_layers_; i++)
    {
        LayerState s = layers_[i].GetState();
        if(s == LayerState::Playing)
            any_playing = true;
        else if(s == LayerState::Paused)
            any_paused = true;
        else if(s == LayerState::ArmedCountIn || s == LayerState::ArmedOverdubCountIn)
            any_count_in = true;
    }
    // Sequencer tracks light the same way: playing green, paused blue,
    // counting in a blue flash, recording red (below).
    bool any_seq_recording = false;
    for(int t = 0; seq_ && t < MidiSequencer::kTracks; t++)
    {
        MidiSequencer::State s = seq_->GetState(t);
        if(s == MidiSequencer::State::Playing)
            any_playing = true;
        else if(s == MidiSequencer::State::Paused)
            any_paused = true;
        else if(s == MidiSequencer::State::Armed)
            any_count_in = true;
        else if(s == MidiSequencer::State::Recording)
            any_seq_recording = true;
    }
    // Count-in (record or overdub armed, waiting for the bar): LED1
    // flashes blue in time with the click, lit for the first half of each
    // beat, taking priority over the playing/paused colour. Stops the
    // moment recording starts, when LED2 takes over in red.
    bool count_in_flash_on = false;
    if(any_count_in)
    {
        int      beat = tempo_->GetBeatInBar();
        uint32_t now  = System::GetNow();
        if(beat != led_count_in_beat_)
        {
            led_count_in_beat_    = beat;
            led_count_in_beat_ms_ = now;
        }
        float beat_ms     = 60000.f / tempo_->GetBpm();
        count_in_flash_on = (float)(now - led_count_in_beat_ms_) < beat_ms * 0.5f;
    }
    else
        led_count_in_beat_ = -1; // next count-in starts lit on its first beat
    if(!looper_enabled_)
    {
        led1_green_pwm_->Set(0.f);
        led1_blue_pwm_->Set(0.f);
    }
    else if(any_count_in)
    {
        led1_green_pwm_->Set(0.f);
        led1_blue_pwm_->Set(count_in_flash_on ? kLedPwmLevel : 0.f);
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
    bool recording = any_loop_recording || any_seq_recording
                      || (live_ && live_->state == LiveExport::kRecording)
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
        case Screen::Arp: DrawArpScreen(); break;
        case Screen::Dexed: DrawDexedScreen(); break;
        case Screen::Ensemble: DrawEnsembleScreen(); break;
        case Screen::Supersaw: DrawSupersawScreen(); break;
        case Screen::DexedOperator: DrawDexedOperatorScreen(); break;
        case Screen::SampleSynth: DrawSampleSynthScreen(); break;
        case Screen::SeqTrack: DrawSeqTrackScreen(); break;
    }
    // Panic confirmation, over whatever page is showing.
    if(panic_shown_ms_ != 0)
    {
        if(System::GetNow() - panic_shown_ms_ < kPanicShowMs)
        {
            const char* msg = "ALL NOTES OFF";
            int         w   = TomThumbAdvanceWidth(msg);
            int         x0  = (disp_->Width() - w) / 2 - 4;
            disp_->DrawRect(x0, 22, x0 + w + 7, 34, true, false);
            disp_->DrawRect(x0 + 1, 23, x0 + w + 6, 33, false, true);
            TomThumbDrawText(disp_, x0 + 4, 31, msg, true);
        }
        else
            panic_shown_ms_ = 0;
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
                         const char* right_label,
                         bool       show_icon)
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

    if(!show_icon)
        return;
    if(square_icon)
        disp_->DrawRect(icon_cx - kIconR, icon_cy - kIconR, icon_cx + kIconR,
                         icon_cy + kIconR, true, false);
    else
        disp_->DrawCircle(icon_cx, icon_cy, kIconR, true);
}

void Ui::DrawGlobalEntryPage(const char*    title,
                             bool           enabled,
                             const uint8_t* icon,
                             const char*    status_right,
                             const char*    b2_label)
{
    // Shared by every engine's (and the Looper's) Global entry page:
    // Enabled/Disabled on the top line (plus an optional right-hand
    // note), the page's graphic centred in the body, and the same
    // encoder hint underneath as Global:Mixer.
    disp_->SetCursor(0, 0);
    WriteUpper(title);
    DrawBeatIndicator(disp_->Width() - 41, 0, 3);
    disp_->DrawLine(0, 9, disp_->Width() - 1, 9, true);

    TomThumbDrawText(disp_, 1, 16, enabled ? "ENABLED" : "DISABLED", true);
    if(status_right[0] != '\0')
        TomThumbDrawText(disp_, disp_->Width() - 1 - TomThumbAdvanceWidth(status_right), 16,
                          status_right, true);

    // Rows 18..37: one clear row under the status line (baseline 16) and
    // two above the hint's cap height (rows 40..44).
    DrawBitmap(disp_, (disp_->Width() - kGlobalIconW) / 2, 18, kGlobalIconW, kGlobalIconH, icon);

    const char* hint = "PUSH ENC TO OPEN";
    TomThumbDrawText(disp_, (disp_->Width() - TomThumbAdvanceWidth(hint)) / 2, 44, hint, true);

    DrawControlRow(kFooterRow1Y, false, kFooterDividerY, "", "", "", "");
    DrawControlRow(kFooterRow2Y, true, kFooterInterRowDividerY, "Toggle On/Off", "", "", b2_label);
}

void Ui::SetMasterVolume01(float v01)
{
    // Same 0..1.43x curve the original firmware used for master volume,
    // so overall loudness feels consistent.
    master_volume01_ = Clampf(v01, 0.f, 1.f);
    master_volume_   = powf(master_volume01_, 2.5f) * 1.43f;
    if(master_volume_ < 0.f)
        master_volume_ = 0.f;
}

void Ui::DrawMetronome(int x, int y, int swing)
{
    // 19x20 body, 1px outlines like the Global icons: a tapered case on a
    // two-row base, the arm pivoting near the bottom with a sliding
    // weight. swing -1/0/+1 = arm leaning left/upright/right.
    disp_->DrawLine(x + 6, y, x + 12, y, true);
    disp_->DrawLine(x + 6, y, x + 1, y + 18, true);
    disp_->DrawLine(x + 12, y, x + 17, y + 18, true);
    disp_->DrawLine(x, y + 18, x + 18, y + 18, true);
    disp_->DrawLine(x, y + 19, x + 18, y + 19, true);
    const int pivot_x = x + 9, pivot_y = y + 16;
    // Arm 18px long, weight 12px up it; leaning = about 23 degrees.
    const int arm_dx = 7 * swing, arm_dy = swing ? 17 : 18;
    const int wt_dx = 5 * swing, wt_dy = swing ? 11 : 12;
    disp_->DrawLine(pivot_x, pivot_y, pivot_x + arm_dx, pivot_y - arm_dy, true);
    disp_->DrawRect(pivot_x + wt_dx - 2, pivot_y - wt_dy - 1, pivot_x + wt_dx + 2,
                    pivot_y - wt_dy + 1, true, true);
}

void Ui::DrawStateIcon(int x, int y, StateIcon icon)
{
    switch(icon)
    {
        case StateIcon::Empty: disp_->DrawLine(x, y + 3, x + 3, y + 3, true); break;
        case StateIcon::Play:
            for(int c = 0; c < 4; c++)
                disp_->DrawLine(x + c, y + c, x + c, y + 6 - c, true);
            break;
        case StateIcon::Pause:
            disp_->DrawRect(x, y, x + 1, y + 6, true, true);
            disp_->DrawRect(x + 3, y, x + 4, y + 6, true, true);
            break;
        case StateIcon::Rec:
        case StateIcon::CountIn:
            for(int dy = -3; dy <= 3; dy++)
                for(int dx = -3; dx <= 3; dx++)
                {
                    const int d = dx * dx + dy * dy;
                    if(icon == StateIcon::Rec ? d <= 10 : (d >= 5 && d <= 10))
                        disp_->DrawPixel(x + 3 + dx, y + 3 + dy, true);
                }
            break;
    }
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
    if(HomeScenes())
    {
        DrawHomeScenePage();
        return;
    }
    if(HomeSong())
    {
        DrawHomeSongPage();
        return;
    }
    if(HomeSeqTrack() >= 0)
    {
        DrawHomeSeqPage();
        return;
    }
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

        // Top left, like the MIDI tracks' boxes: A1..A4 and the state glyph.
        char label[8];
        snprintf(label, sizeof(label), "A%d", i + 1);
        disp_->SetCursor(x0 + 3, top + 4);
        WriteUpper(label);
        StateIcon icon = StateIcon::Empty;
        switch(layers_[i].GetState())
        {
            case LayerState::Empty: icon = StateIcon::Empty; break;
            case LayerState::Playing: icon = StateIcon::Play; break;
            case LayerState::Paused: icon = StateIcon::Pause; break;
            case LayerState::Recording:
            case LayerState::Overdubbing: icon = StateIcon::Rec; break;
            case LayerState::ArmedCountIn:
            case LayerState::ArmedOverdubCountIn: icon = StateIcon::CountIn; break;
        }
        DrawStateIcon(x0 + 16, top + 4, icon);
        // A playhead along the bottom, as on a MIDI track's box.
        if(layers_[i].HasContent())
        {
            int px = x0 + 2 + (int)(layers_[i].GetPlayPos01() * (float)(box_w - 5));
            disp_->DrawLine(px, y1 - 2, px, y1 - 1, true);
        }
    }
    // What the boxes are, in the gap above the footer.
    const char* what = "Audio Tracks";
    TomThumbDrawText(disp_, (disp_->Width() - TomThumbAdvanceWidth(what)) / 2, 44, what, true);

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
    DrawControlRow(kFooterRow1Y, false, kFooterDividerY, "Master Vol", vol_val, metro_val,
                     "Metro");

    // Button1's label reflects what a tap/hold will actually do right now
    // (see LooperLayer::OnRecordButtonPressed/LongPress) rather than a
    // static "Rec/Play" covering three different behaviours -- once
    // there's content, a tap toggles Pause/Play and a hold starts an
    // overdub, both unchanged; this only changes what the label says.
    const char* rec_label = "Rec";
    switch(layers_[cursor_layer_].GetState())
    {
        // Short-tap action / hold action, the hold spelled out so it's
        // clear recording needs one -- the same words on a layer's Status
        // page and a sequencer track (SeqB1Label()).
        case LayerState::Empty: rec_label = "Hold=Rec"; break;
        case LayerState::Playing: rec_label = "Pause/Hold=Rec"; break;
        case LayerState::Paused: rec_label = "Play/Hold=Rec"; break;
        case LayerState::ArmedCountIn: rec_label = "Cancel"; break;
        case LayerState::Recording: rec_label = "Stop"; break;
        // A tap now ends overdub early (same as Recording's own "Stop"
        // above), now that overdub runs a full pass automatically
        // instead of lasting exactly as long as the button stays held.
        case LayerState::Overdubbing: rec_label = "Stop"; break;
        case LayerState::ArmedOverdubCountIn: rec_label = "Cancel"; break;
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
void Ui::SeqRecord(int t)
{
    if(!seq_ || t < 0 || !MidiSequencer::CanArm(seq_->GetState(t)))
        return;
    // Arming counts in exactly like a layer: the tempo clock waits for the
    // next bar, clicks 4 beats, and the track starts on the downbeat after.
    tempo_->RequestRecordStart();
    seq_->RequestRecord(t);
}

const char* Ui::SeqB1Label(int t) const
{
    // The same words as a layer's B1 (see DrawHome()).
    switch(seq_->GetState(t))
    {
        case MidiSequencer::State::Empty: return "Hold=Rec";
        case MidiSequencer::State::Playing: return "Pause/Hold=Rec";
        case MidiSequencer::State::Paused: return "Play/Hold=Rec";
        case MidiSequencer::State::Armed: return "Cancel";
        case MidiSequencer::State::Recording: return "Stop";
    }
    return "";
}

// MIDI note name with middle C (60) = C4, so the kits' first pad (36)
// reads C2 as the README says.
static void SeqNoteName(int note, char* out, size_t out_size)
{
    static const char* const kNames[12]
        = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
    snprintf(out, out_size, "%s%d", kNames[note % 12], note / 12 - 1);
}

uint32_t Ui::SeqGridPageTicks(uint32_t step)
{
    const uint32_t bar = 4 * MidiSequencer::kPpq;
    return bar / step > 24 ? bar / 2 : bar;
}

int Ui::SeqGridBars(int t) const
{
    if(!seq_ || t < 0)
        return 1;
    const uint32_t len  = MidiSequencer::TrackTicksFor(seq_->GetLengthIndex(t), tempo_->GetBars());
    const uint32_t page = SeqGridPageTicks(MidiSequencer::GridTicks(seq_->GetQuantIndex(t)));
    return len > page ? (int)((len + page - 1) / page) : 1;
}

Ui::SeqGridView Ui::SeqGridGeometry(int t) const
{
    SeqGridView v{};
    v.len_ticks        = MidiSequencer::TrackTicksFor(seq_->GetLengthIndex(t), tempo_->GetBars());
    v.step             = MidiSequencer::GridTicks(seq_->GetQuantIndex(t));
    const uint32_t pg  = SeqGridPageTicks(v.step);
    int page           = seq_page_ - kSeqPageGrid;
    if(page < 0)
        page = 0;
    v.page_start       = (uint32_t)page * pg;
    if(v.page_start >= v.len_ticks)
        v.page_start = 0;
    uint32_t span    = v.len_ticks - v.page_start < pg ? v.len_ticks - v.page_start : pg;
    v.cells          = (int)(span / v.step);
    v.cells_per_page = (int)(pg / v.step);
    // SMPDR kits: pad 1 is C2 and they go up from there, top row first;
    // every other engine is a piano roll, five octaves C1..C6, highest at
    // the top (about 4.4 degrees of K2 per note).
    v.drum    = seq_->GetEngine(t) == MidiSequencer::kSmpdr;
    v.note_lo = v.drum ? 36 : 24;
    v.note_hi = v.drum ? 36 + 23 : 84;
    return v;
}

bool Ui::SeqFindNoteAt(int t, const SeqGridView& v, int col, int note, int* vel, int* roll,
                       uint32_t* len) const
{
    // A note belongs to the cell its start is nearest (the same rounding
    // quantise uses).
    // col -1 (the row position): the first note of that pitch anywhere.
    const bool                   any  = col < 0;
    const int32_t                c0   = (int32_t)(v.page_start + (uint32_t)(any ? 0 : col) * v.step);
    const int32_t                from = c0 - (int32_t)v.step / 2, to = c0 + (int32_t)v.step / 2;
    const MidiSequencer::Event*  ev   = seq_->GetEvents(t);
    const int                    n    = seq_->GetEventCount(t);
    for(int i = 0; i < n; i++)
    {
        if(ev[i].velocity == 0 || ev[i].note != note)
            continue;
        int32_t tk = (int32_t)ev[i].tick;
        if(!any && !((tk >= from && tk < to) || (from < 0 && tk >= from + (int32_t)v.len_ticks)))
            continue;
        *vel  = ev[i].velocity;
        *roll = ev[i].roll < 2 ? 1 : ev[i].roll;
        *len  = v.step;
        for(int k = 1; k < n; k++) // its note off: the next one for this note
        {
            const MidiSequencer::Event& e = ev[(i + k) % n];
            if(e.note != note)
                continue;
            if(e.velocity == 0)
                *len = (e.tick + v.len_ticks - ev[i].tick) % v.len_ticks;
            break;
        }
        return true;
    }
    return false;
}

void Ui::SeqGridToggle(int t)
{
    if(!seq_ || t < 0)
        return;
    SeqGridView v = SeqGridGeometry(t);
    if(grid_col_ < 0 || grid_col_ >= v.cells)
        return; // the row position adds nothing
    int      vel, roll;
    uint32_t len;
    const int32_t c0 = (int32_t)(v.page_start + (uint32_t)grid_col_ * v.step);
    if(SeqFindNoteAt(t, v, grid_col_, grid_note_, &vel, &roll, &len))
        seq_->RequestDeleteNote(t, c0 - (int32_t)v.step / 2, c0 + (int32_t)v.step / 2,
                                (uint8_t)grid_note_, v.len_ticks);
    else
    {
        // A drum hit is held half a step; a pitched note a whole step.
        seq_->RequestAddNote(t, (uint32_t)c0, (uint8_t)grid_note_, 100,
                             v.drum ? v.step / 2 : v.step, v.len_ticks);
        Preview(seq_->GetEngine(t), grid_note_, 100); // hear what went in
    }
}

void Ui::Preview(int engine, int note, int velocity)
{
    preview_engine_ = engine;
    preview_note_   = note;
    preview_vel_    = velocity;
    preview_seq_    = preview_seq_ + 1; // last: main.cpp reads the rest after seeing it
}

void Ui::SeqGridKnobs(int t, float k1, float k2)
{
    SeqGridView   v  = SeqGridGeometry(t);
    // On the row position, edits go to every note of this pitch.
    const bool    row  = grid_col_ < 0;
    const int32_t c0   = (int32_t)(v.page_start + (uint32_t)(row ? 0 : grid_col_) * v.step);
    const int32_t from = row ? MidiSequencer::kWholeRow : c0 - (int32_t)v.step / 2;
    const int32_t to   = c0 + (int32_t)v.step / 2;
    const bool    held = pod_->button2.Pressed();

    if(held && !grid_edit_)
    {
        // B2 down: edit the note under the cursor, the knobs relative to
        // where they are now (so nothing jumps).
        int      vel = 100, roll = 1;
        uint32_t len = v.step;
        grid_edit_      = true;
        grid_edit_note_ = SeqFindNoteAt(t, v, grid_col_, grid_note_, &vel, &roll, &len);
        grid_edit_k1_on_ = false;
        grid_edit_k2_on_ = false;
        grid_edit_v1_ref_ = v.drum ? vel : (int)((len + v.step / 2) / v.step);
        if(grid_edit_v1_ref_ < 1)
            grid_edit_v1_ref_ = 1;
        int ri = 0;
        while(ri < MidiSequencer::kNumRolls - 1 && MidiSequencer::RollCount(ri) < roll)
            ri++;
        grid_edit_roll_ref_ = ri;
        grid_edit_v1_       = grid_edit_v1_ref_;
        grid_edit_roll_     = ri;
        grid_edit_k1_below_ = k1 < SeqEditK1Target(v);
        grid_edit_k2_below_ = k2 < ((float)ri + 0.5f) / (float)MidiSequencer::kNumRolls;
    }
    if(grid_edit_)
    {
        if(!held)
        {
            // Let go: the knobs go back to the cursor, picked up from it.
            grid_edit_            = false;
            size_t ci             = (size_t)KnobContext::SeqGrid;
            k1_pickup_engaged_[ci] = false;
            k2_pickup_engaged_[ci] = false;
            SyncPickupTargets(KnobContext::SeqGrid);
            return;
        }
        if(!grid_edit_note_)
            return;
        // Pick up: follow each knob once it's turned through (or onto)
        // the value it edits.
        const float t1 = SeqEditK1Target(v);
        const float t2 = ((float)grid_edit_roll_ + 0.5f) / (float)MidiSequencer::kNumRolls;
        if(!grid_edit_k1_on_ && (fabsf(k1 - t1) < 0.04f || (k1 < t1) != grid_edit_k1_below_))
            grid_edit_k1_on_ = true;
        if(!grid_edit_k2_on_ && (fabsf(k2 - t2) < 0.04f || (k2 < t2) != grid_edit_k2_below_))
            grid_edit_k2_on_ = true;
        const int v1_max = v.drum ? 127 : (int)(v.len_ticks / v.step);
        int       v1     = grid_edit_v1_;
        if(grid_edit_k1_on_)
            v1 = 1 + (int)(Clampf(k1, 0.f, 1.f) * (float)(v1_max - 1) + 0.5f);
        int ri = grid_edit_roll_;
        if(grid_edit_k2_on_)
            ri = (int)(Clampf(k2, 0.f, 0.999f) * (float)MidiSequencer::kNumRolls);
        if(v1 != grid_edit_v1_)
        {
            grid_edit_v1_ = v1;
            if(v.drum)
                seq_->RequestSetVelocity(t, from, to, (uint8_t)grid_note_, (uint8_t)v1,
                                         v.len_ticks);
            else
                seq_->RequestSetLength(t, from, to, (uint8_t)grid_note_, (uint32_t)v1 * v.step,
                                       v.len_ticks);
        }
        if(ri != grid_edit_roll_)
        {
            grid_edit_roll_ = ri;
            seq_->RequestSetRoll(t, from, to, (uint8_t)grid_note_, MidiSequencer::RollCount(ri),
                                 v.len_ticks);
        }
        return;
    }

    // The cursor: K1 the step, K2 the row (picked up, so arriving here
    // or letting go of B2 doesn't move it).
    if(PickUpK1(k1) && v.cells > 0)
        grid_col_ = (int)(Clampf(k1, 0.f, 0.999f) * (float)(v.cells + 1)) - 1;
    if(PickUpK2(k2))
    {
        // Sticky: the row only changes once the knob is a third of a row
        // past the edge of the current one, so pot jitter can't flicker it
        // between neighbours.
        const int   rows = v.note_hi - v.note_lo + 1;
        const float f    = Clampf(k2, 0.f, 0.999f) * (float)rows;
        const int   cur  = v.drum ? grid_note_ - v.note_lo : v.note_hi - grid_note_;
        if(f < (float)cur - 0.33f || f > (float)(cur + 1) + 0.33f || cur < 0 || cur >= rows)
        {
            int row_idx = (int)f;
            grid_note_  = v.drum ? v.note_lo + row_idx : v.note_hi - row_idx;
        }
    }
    if(grid_col_ >= v.cells)
        grid_col_ = v.cells > 0 ? v.cells - 1 : 0;
}

float Ui::SeqEditK1Target(const SeqGridView& v) const
{
    // Where K1 sits for the value it edits: velocity 1..127, or length
    // 1..the whole track in steps.
    const int v1_max = v.drum ? 127 : (int)(v.len_ticks / v.step);
    return v1_max > 1 ? (float)(grid_edit_v1_ - 1) / (float)(v1_max - 1) : 0.f;
}

void Ui::DrawSeqGridPage(int t)
{
    constexpr int kRows = 6, kX0 = 21, kRowH = 6, kTop = 11;
    SeqGridView v = SeqGridGeometry(t);
    if(grid_col_ >= v.cells)
        grid_col_ = v.cells > 0 ? v.cells - 1 : 0;

    char title[16];
    // "Bar2/4", or "Bar2b/4" for the second half of bar 2 on a half-bar
    // page (1/32, 1/32T).
    const uint32_t bar_ticks = 4 * MidiSequencer::kPpq;
    const int      bars      = (int)((v.len_ticks + bar_ticks - 1) / bar_ticks);
    const bool     half      = SeqGridPageTicks(v.step) < bar_ticks;
    snprintf(title, sizeof(title), "M%d:Bar%d%s/%d", t + 1, (int)(v.page_start / bar_ticks) + 1,
              !half ? "" : (v.page_start % bar_ticks) ? "b" : "a", bars > 0 ? bars : 1);
    disp_->SetCursor(0, 0);
    WriteUpper(title);
    DrawBeatIndicator(disp_->Width() - 41, 0, 3);
    disp_->DrawLine(0, 9, disp_->Width() - 1, 9, true);

    // Keep the cursor's row on screen.
    if(v.drum)
    {
        if(grid_note_ < grid_top_)
            grid_top_ = grid_note_;
        if(grid_note_ > grid_top_ + kRows - 1)
            grid_top_ = grid_note_ - kRows + 1;
    }
    else
    {
        if(grid_note_ > grid_top_)
            grid_top_ = grid_note_;
        if(grid_note_ < grid_top_ - kRows + 1)
            grid_top_ = grid_note_ + kRows - 1;
    }
    const int cw = 96 / v.cells_per_page; // cell width, 4..24 px

    // Each row's notes, from one pass over the track.
    int8_t   vel[kRows][24];
    int8_t   roll[kRows][24];
    uint8_t  span[kRows][24]; // instrument: cells the note lasts
    memset(vel, 0, sizeof(vel));
    memset(roll, 0, sizeof(roll));
    memset(span, 0, sizeof(span));
    const MidiSequencer::Event* ev = seq_->GetEvents(t);
    const int                   n  = seq_->GetEventCount(t);
    const uint32_t total_cells     = v.len_ticks / v.step;
    for(int i = 0; i < n && total_cells > 0; i++)
    {
        if(ev[i].velocity == 0)
            continue;
        int row = v.drum ? ev[i].note - grid_top_ : grid_top_ - ev[i].note;
        if(row < 0 || row >= kRows)
            continue;
        int gc  = (int)(((ev[i].tick + v.step / 2) / v.step) % total_cells);
        int col = gc - (int)(v.page_start / v.step);
        int len_cells = 1;
        if(!v.drum)
            for(int k = 1; k < n; k++)
            {
                const MidiSequencer::Event& e = ev[(i + k) % n];
                if(e.note != ev[i].note)
                    continue;
                if(e.velocity == 0)
                {
                    uint32_t ln = (e.tick + v.len_ticks - ev[i].tick) % v.len_ticks;
                    len_cells   = (int)((ln + v.step / 2) / v.step);
                    if(len_cells < 1)
                        len_cells = 1;
                }
                break;
            }
        for(int c = col; c < col + len_cells && c < v.cells; c++)
        {
            if(c < 0)
                continue;
            if(c == col)
            {
                vel[row][c]  = (int8_t)ev[i].velocity;
                roll[row][c] = (int8_t)ev[i].roll;
            }
            else
                span[row][c] = 1;
        }
    }

    for(int r = 0; r < kRows; r++)
    {
        const int note = v.drum ? grid_top_ + r : grid_top_ - r;
        if(note < v.note_lo || note > v.note_hi)
            continue;
        const int y = kTop + r * kRowH;
        char      name[6];
        SeqNoteName(note, name, sizeof(name));
        const bool row_sel = grid_col_ < 0 && note == grid_note_;
        if(row_sel)
            disp_->DrawRect(0, y - 1, kX0 - 3, y + 5, true, true);
        TomThumbDrawText(disp_, 1, y + 5, name, !row_sel);
        // Black keys on the piano roll: a dotted line under the row.
        static const bool kBlack[12] = {0, 1, 0, 1, 0, 0, 1, 0, 1, 0, 1, 0};
        if(!v.drum && kBlack[note % 12])
            for(int x = kX0; x < kX0 + v.cells * cw; x += 2)
                disp_->DrawPixel(x, y + 5, true);
        for(int c = 0; c < v.cells; c++)
        {
            const int x = kX0 + c * cw;
            if(vel[r][c] > 0)
            {
                disp_->DrawRect(x, y, x + cw - 2, y + 4, true, true);
                // A roll: notches cut through the hit, one per repeat.
                int rr = roll[r][c];
                for(int k = 1; rr > 1 && k < rr && cw >= 4; k++)
                    disp_->DrawLine(x + k * (cw - 1) / rr, y + 1, x + k * (cw - 1) / rr, y + 3,
                                    false);
            }
            else if(span[r][c])
                disp_->DrawRect(x - 1, y + 1, x + cw - 2, y + 3, true, true); // held on
            else
                disp_->DrawPixel(x + cw / 2 - 1, y + 2, true);
        }
    }
    // Beat lines.
    const int cells_per_beat = (int)(MidiSequencer::kPpq / v.step);
    for(int c = cells_per_beat; cells_per_beat > 0 && c < v.cells; c += cells_per_beat)
        for(int y = kTop; y < kTop + kRows * kRowH - 1; y += 2)
            disp_->DrawPixel(kX0 + c * cw - 1, y, true);

    // The arp's notes (its pattern's arp on), where and as long as it plays
    // each: a hollow bar in the open, a line cut along a recorded key it
    // lies across (so the key stays whole).
    {
        const Arpeggiator::RenderedNote* ghost;
        const int                        gn = RenderTrackArp(t, &ghost);
        const int32_t page_end = (int32_t)(v.page_start + (uint32_t)v.cells * v.step);
        for(int i = 0; i < gn; i++)
        {
            const int r = v.drum ? ghost[i].note - grid_top_ : grid_top_ - ghost[i].note;
            if(r < 0 || r >= kRows)
                continue;
            // Its span, and a copy a loop on for one wrapping round the top.
            for(int lap = 0; lap < 2; lap++)
            {
                int32_t a = (int32_t)ghost[i].tick - (lap ? (int32_t)v.len_ticks : 0);
                int32_t b = a + (int32_t)(ghost[i].length < 1 ? 1 : ghost[i].length);
                if(b <= (int32_t)v.page_start || a >= page_end)
                    continue;
                a = a < (int32_t)v.page_start ? (int32_t)v.page_start : a;
                b = b > page_end ? page_end : b;
                int x0 = kX0 + (int)((a - (int32_t)v.page_start) * cw / (int32_t)v.step);
                int x1 = kX0 + (int)((b - (int32_t)v.page_start) * cw / (int32_t)v.step) - 2;
                x1     = x1 < x0 + 1 ? x0 + 1 : x1;
                const int y = kTop + r * kRowH;
                auto on_key = [&](int x) {
                    const int c = (x - kX0) / cw;
                    return c >= 0 && c < v.cells && (vel[r][c] > 0 || span[r][c]);
                };
                for(int x = x0; x <= x1; x++)
                {
                    if(on_key(x)) // through a recorded key: a line cut along its middle
                        disp_->DrawPixel(x, y + 2, false);
                    else // in the open: a hollow bar
                    {
                        disp_->DrawPixel(x, y + 1, true);
                        disp_->DrawPixel(x, y + 3, true);
                    }
                }
                if(!on_key(x0))
                    disp_->DrawPixel(x0, y + 2, true);
                if(!on_key(x1))
                    disp_->DrawPixel(x1, y + 2, true);
            }
        }
    }

    // The cursor: a hit under it is drawn hollow, an empty step boxed
    // (on the row position its name is highlighted instead, above).
    if(grid_col_ >= 0)
    {
        int r = v.drum ? grid_note_ - grid_top_ : grid_top_ - grid_note_;
        int x = kX0 + grid_col_ * cw, y = kTop + r * kRowH;
        if(vel[r][grid_col_] > 0)
            disp_->DrawRect(x + 1, y + 1, x + cw - 3, y + 3, false, true);
        else
            disp_->DrawRect(x - 1, y - 1, x + cw - 1, y + 5, true, false);
    }
    // Playhead, above the rows.
    {
        uint32_t pos = (uint32_t)(seq_->GetPosition01(t) * (float)v.len_ticks);
        if(seq_->GetEventCount(t) > 0 && pos >= v.page_start && pos < v.page_start + v.cells * v.step)
        {
            int x = kX0 + (int)((pos - v.page_start) / v.step) * cw + cw / 2 - 1;
            disp_->DrawLine(x, 10, x + 1, 10, true);
        }
    }

    char a[8], b[8];
    if(grid_edit_ && grid_edit_note_)
    {
        // B2 held: the knobs edit the note under the cursor.
        snprintf(a, sizeof(a), "%d", grid_edit_v1_);
        snprintf(b, sizeof(b), "x%d", MidiSequencer::RollCount(grid_edit_roll_));
        DrawControlRow(kFooterRow1Y, false, kFooterDividerY,
                         grid_col_ < 0 ? (v.drum ? "Row Vel" : "Row Len") : (v.drum ? "Vel" : "Len"),
                         a, b, "Roll");
    }
    else
    {
        if(grid_col_ < 0)
            snprintf(a, sizeof(a), "Row");
        else
            snprintf(a, sizeof(a), "%d/%d", grid_col_ + 1, v.cells);
        SeqNoteName(grid_note_, b, sizeof(b));
        DrawControlRow(kFooterRow1Y, false, kFooterDividerY, "Step", a, b, v.drum ? "Pad" : "Note");
    }
    DrawControlRow(kFooterRow2Y, true, kFooterInterRowDividerY, "Add/Del", "", "",
                     v.drum ? "Hold=Vel/Roll" : "Hold=Len/Roll");
}

uint32_t Ui::SeqPrintTracks(int t, uint32_t* engines) const
{
    uint32_t tracks = 0, eng = 0;
    for(int k = 0; k < MidiSequencer::kTracks; k++)
    {
        if(seq_->GetEventCount(k) == 0)
            continue;
        // A track shares its engine's one output, so a whole engine prints.
        if(seq_print_all_ || seq_->GetEngine(k) == seq_->GetEngine(t))
        {
            tracks |= 1u << k;
            eng |= 1u << seq_->GetEngine(k);
        }
    }
    // Every other track on a printed engine goes with it.
    for(int k = 0; k < MidiSequencer::kTracks; k++)
        if(seq_->GetEventCount(k) > 0 && (eng & (1u << seq_->GetEngine(k))))
            tracks |= 1u << k;
    if(engines)
        *engines = eng;
    return tracks;
}

void Ui::DrawSeqChainPage(int t)
{
    char title[16];
    snprintf(title, sizeof(title), "M%d:Chain", t + 1);
    disp_->SetCursor(0, 0);
    WriteUpper(title);
    DrawBeatIndicator(disp_->Width() - 41, 0, 3);
    disp_->DrawLine(0, 9, disp_->Width() - 1, 9, true);

    if(pod_->button2.Pressed() && pod_->button2.TimeHeldMs() > 200.f && !button2_long_fired_)
    {
        float held = pod_->button2.TimeHeldMs();
        int   w    = (int)(Clampf(held / 800.f, 0.f, 1.f) * (disp_->Width() - 2));
        disp_->SetCursor(0, 20);
        WriteUpper("Hold: Clear chain...");
        disp_->DrawRect(0, 30, disp_->Width() - 1, 34, true, false);
        if(w > 0)
            disp_->DrawRect(1, 31, w, 33, true, true);
        DrawControlRow(kFooterRow1Y, false, kFooterDividerY, "", "", "", "");
        DrawControlRow(kFooterRow2Y, true, kFooterInterRowDividerY, "", "", "", "");
        return;
    }

    // A box per step: filled = the one playing, dotted = room for more.
    // Until it takes over (the loop's next start) the steps blink.
    int8_t     ch[MidiSequencer::kMaxChain];
    const int  n       = seq_->GetChain(t, ch);
    const int  step    = seq_->GetChainStep(t);
    const bool pending = seq_->ChainPending(t);
    const bool blink   = (System::GetNow() / 300) % 2;
    for(int k = 0; k < MidiSequencer::kMaxChain; k++)
    {
        const int x0 = 4 + k * 15, x1 = x0 + 12, y0 = 13, y1 = 27;
        if(k >= n)
        {
            for(int x = x0; x <= x1; x += 2)
            {
                disp_->DrawPixel(x, y0, true);
                disp_->DrawPixel(x, y1, true);
            }
            continue;
        }
        const bool fill = !pending && k == step;
        if(fill)
            disp_->DrawRect(x0, y0, x1, y1, true, true);
        else if(!pending || blink)
            disp_->DrawRect(x0, y0, x1, y1, true, false);
        char pl[2] = {(char)('A' + ch[k]), 0};
        disp_->SetCursor(x0 + 4, y0 + 4);
        disp_->WriteString(pl, Font_6x8, !fill);
    }
    const char* line = n >= 2 ? (pending ? "Starts with the loop" : "Chain playing")
                       : n == 1 ? "Add more to chain"
                                : "Tap B2 to add patterns";
    TomThumbDrawText(disp_, 0, 39, line, true);

    char cur[2] = {(char)('A' + seq_patt_cursor_), 0};
    DrawControlRow(kFooterRow1Y, false, kFooterDividerY, "Pattern", cur, "", "");
    DrawControlRow(kFooterRow2Y, true, kFooterInterRowDividerY, SeqB1Label(t), "", "",
                   n > 0 ? "Add/Hold=Clear" : "Add");
}

void Ui::DrawSeqPattPage(int t)
{
    char title[16];
    snprintf(title, sizeof(title), "M%d:Patt", t + 1);
    disp_->SetCursor(0, 0);
    WriteUpper(title);
    DrawBeatIndicator(disp_->Width() - 41, 0, 3);
    disp_->DrawLine(0, 9, disp_->Width() - 1, 9, true);

    // Held B2 (past a tap, before it fires): the usual hold-to-confirm bar.
    if(pod_->button2.Pressed() && pod_->button2.TimeHeldMs() > 200.f && !button2_long_fired_)
    {
        float held = pod_->button2.TimeHeldMs();
        int   w    = (int)(Clampf(held / 800.f, 0.f, 1.f) * (disp_->Width() - 2));
        char  msg[24];
        snprintf(msg, sizeof(msg), "Hold: Copy %c to %c...", 'A' + seq_->GetPattern(t),
                  'A' + seq_patt_cursor_);
        disp_->SetCursor(0, 20);
        WriteUpper(msg);
        disp_->DrawRect(0, 30, disp_->Width() - 1, 34, true, false);
        if(w > 0)
            disp_->DrawRect(1, 31, w, 33, true, true);
        DrawControlRow(kFooterRow1Y, false, kFooterDividerY, "", "", "", "");
        DrawControlRow(kFooterRow2Y, true, kFooterInterRowDividerY, "", "", "", "");
        return;
    }

    // A box per pattern: filled = playing, outlined = has notes, dotted =
    // empty; the queued one blinks, the K1 choice has a mark under it.
    const int playing = seq_->GetPattern(t);
    const int next    = seq_->GetNextPattern(t);
    const bool blink  = (System::GetNow() / 300) % 2;
    for(int p = 0; p < MidiSequencer::kPatterns; p++)
    {
        const int x0 = 4 + p * 15, x1 = x0 + 12, y0 = 13, y1 = 27;
        const bool notes = seq_->PatternHasNotes(t, p);
        const bool fill  = p == playing || (p == next && blink);
        if(fill)
            disp_->DrawRect(x0, y0, x1, y1, true, true);
        else if(notes)
            disp_->DrawRect(x0, y0, x1, y1, true, false);
        else
            for(int x = x0; x <= x1; x += 2)
            {
                disp_->DrawPixel(x, y0, true);
                disp_->DrawPixel(x, y1, true);
            }
        char pl[2] = {(char)('A' + p), 0};
        disp_->SetCursor(x0 + 4, y0 + 4);
        disp_->WriteString(pl, Font_6x8, !fill);
        if(p == seq_patt_cursor_)
            disp_->DrawRect(x0, y1 + 2, x1, y1 + 3, true, true);
    }
    char      line[32];
    const int tp = seq_->GetTranspose(t);
    if(next >= 0)
        snprintf(line, sizeof(line), "Playing %c, %c next", 'A' + playing, 'A' + next);
    else if(tp != 0)
        snprintf(line, sizeof(line), "Playing %c, %+d semitones", 'A' + playing, tp);
    else
        snprintf(line, sizeof(line), "Playing %c", 'A' + playing);
    TomThumbDrawText(disp_, 0, 39, line, true);

    char cur[2] = {(char)('A' + seq_patt_cursor_), 0};
    char tps[6];
    snprintf(tps, sizeof(tps), "%+d", seq_->GetTranspose(t, seq_patt_cursor_));
    DrawControlRow(kFooterRow1Y, false, kFooterDividerY, "Pattern", cur, tps, "Transp");
    DrawControlRow(kFooterRow2Y, true, kFooterInterRowDividerY, SeqB1Label(t), "", "",
                     "Play/Hold=Copy");
}

bool Ui::SeqPrintFits(int t) const
{
    // A layer is one loop long, so every track printed has to fit in it.
    const uint32_t loop   = (uint32_t)tempo_->GetBars() * 4 * MidiSequencer::kPpq;
    uint32_t       tracks = SeqPrintTracks(t, nullptr) | (1u << t);
    for(int k = 0; k < MidiSequencer::kTracks; k++)
        if((tracks & (1u << k))
           && MidiSequencer::TrackTicksFor(seq_->GetLengthIndex(k), tempo_->GetBars()) > loop)
            return false;
    return true;
}

namespace
{
// RenderTrackArp()'s buffers -- SDRAM, constructed on first use (after
// SDRAM's up). Main loop only.
constexpr int kGhostMax = 512;
uint32_t      Fnv(uint32_t h, const void* p, size_t n)
{
    const uint8_t* b = static_cast<const uint8_t*>(p);
    for(size_t i = 0; i < n; i++)
        h = (h ^ b[i]) * 16777619u;
    return h;
}
} // namespace

int Ui::RenderTrackArp(int t, const Arpeggiator::RenderedNote** out)
{
    static MidiSequencer::Event DSY_SDRAM_BSS        play[MidiSequencer::kMaxPlayEvents];
    static MidiSequencer::Event DSY_SDRAM_BSS        scratch[MidiSequencer::kMaxEvents];
    static int16_t DSY_SDRAM_BSS                     partner[MidiSequencer::kMaxEvents];
    static int32_t DSY_SDRAM_BSS                     shift[MidiSequencer::kMaxEvents];
    static Arpeggiator::KeyEvent DSY_SDRAM_BSS       keys[MidiSequencer::kMaxPlayEvents];
    static Arpeggiator::RenderedNote DSY_SDRAM_BSS   notes[kGhostMax];
    static Arpeggiator DSY_SDRAM_BSS                 render_arp; // scratch, not the live arps
    *out = notes;
    if(!seq_ || !arp_ || t < 0)
        return 0;
    const Arpeggiator::Settings& s   = arp_->PeekPattern(t, seq_->GetPattern(t));
    const uint32_t               len = MidiSequencer::TrackTicksFor(seq_->GetLengthIndex(t), tempo_->GetBars());
    if(!s.on || len == 0 || seq_->GetEventCount(t) == 0)
    {
        ghost_sig_ = 0;
        return 0;
    }
    const int n = seq_->BuildPlayCopy(t, len, play, MidiSequencer::kMaxPlayEvents, scratch, partner, shift);
    // Worked out again only when what it plays from, or how, has changed.
    uint32_t sig = 2166136261u;
    sig          = Fnv(sig, &t, sizeof(t));
    sig          = Fnv(sig, &len, sizeof(len));
    sig          = Fnv(sig, &s, sizeof(s));
    sig          = Fnv(sig, play, (size_t)n * sizeof(MidiSequencer::Event));
    sig          = sig ? sig : 1;
    if(sig == ghost_sig_)
        return ghost_count_;
    for(int i = 0; i < n; i++)
        keys[i] = {play[i].tick, play[i].note, play[i].velocity};
    ghost_count_ = render_arp.Render(s, keys, n, len, notes, kGhostMax);
    ghost_sig_   = sig;
    return ghost_count_;
}

void Ui::PrintArpToNotes(int t)
{
    static MidiSequencer::NoteSpan DSY_SDRAM_BSS spans[kGhostMax];
    static MidiSequencer::Event DSY_SDRAM_BSS    events[MidiSequencer::kMaxEvents];
    const int cur = seq_->GetPattern(t);
    if(!arp_->PeekPattern(t, cur).on)
    {
        snprintf(seq_print_status_, sizeof(seq_print_status_), "%c's arp is off", 'A' + cur);
        return;
    }
    int target = -1;
    for(int p = 0; p < MidiSequencer::kPatterns && target < 0; p++)
        if(p != cur && !seq_->PatternHasNotes(t, p))
            target = p;
    if(target < 0)
    {
        snprintf(seq_print_status_, sizeof(seq_print_status_), "No empty pattern");
        return;
    }
    const Arpeggiator::RenderedNote* notes;
    const int                        m = RenderTrackArp(t, &notes);
    if(m == 0)
    {
        snprintf(seq_print_status_, sizeof(seq_print_status_), "The arp plays nothing");
        return;
    }
    for(int i = 0; i < m; i++)
        spans[i] = {notes[i].tick, notes[i].length, notes[i].note, notes[i].velocity};
    const uint32_t len = MidiSequencer::TrackTicksFor(seq_->GetLengthIndex(t), tempo_->GetBars());
    const int      ne  = MidiSequencer::NotesToEvents(spans, m, len, events, MidiSequencer::kMaxEvents);
    // The notes as heard: no quantise, swing or transpose on top (the arp's
    // own swing is in them already), the same length; the arp off there.
    MidiSequencer::TrackSave ts{};
    ts.engine       = (uint8_t)seq_->GetEngine(t);
    ts.length_index = (uint8_t)seq_->GetLengthIndex(t);
    seq_->LoadPattern(t, target, ts);
    seq_->AppendPatternEvents(t, target, events, ne);
    arp_->CopyPattern(t, cur, target);
    arp_->PatternSettings(t, target).on = false;
    seq_->RequestPattern(t, target);
    seq_patt_cursor_ = target;
    snprintf(seq_print_status_, sizeof(seq_print_status_), ne / 2 < m ? "Into %c (cut short)" : "Printed into %c",
              'A' + target);
}

void Ui::StartSeqPrint(int t)
{
    if(!seq_ || t < 0 || seq_print_layer_ >= 0)
        return;
    if(seq_print_dest_ >= num_layers_) // K1's last place: the arp, into notes
    {
        PrintArpToNotes(t);
        return;
    }
    if(!SeqPrintFits(t))
    {
        snprintf(seq_print_status_, sizeof(seq_print_status_), "Longer than the loop");
        return;
    }
    LooperLayer& layer = layers_[seq_print_dest_];
    LayerState   st    = layer.GetState();
    if(st != LayerState::Empty && st != LayerState::Playing && st != LayerState::Paused)
    {
        snprintf(seq_print_status_, sizeof(seq_print_status_), "L%d is busy", seq_print_dest_ + 1);
        return;
    }
    // The tracks it takes (and their engines) -- a paused one is started
    // so it's heard.
    uint32_t engines = 0;
    uint32_t tracks  = SeqPrintTracks(t, &engines);
    if(engines == 0)
    {
        snprintf(seq_print_status_, sizeof(seq_print_status_), "Nothing to print");
        return;
    }
    for(int k = 0; k < MidiSequencer::kTracks; k++)
        if(tracks & (1u << k))
            seq_->RequestPlay(k, true);
    // The same count-in and one pass as recording the layer by hand (an
    // overdub if it already holds audio).
    layer.OnRecordButtonLongPress(*tempo_);
    seq_print_mask_     = engines;
    seq_print_tracks_   = tracks;
    seq_print_recorded_ = false;
    seq_print_layer_    = seq_print_dest_;
    seq_print_status_[0] = '\0';
}

void Ui::UpdateSeqPrint()
{
    if(seq_print_layer_ < 0 || !seq_)
        return;
    LayerState st = layers_[seq_print_layer_].GetState();
    if(st == LayerState::Recording || st == LayerState::Overdubbing)
    {
        seq_print_recorded_ = true;
        return;
    }
    if(st == LayerState::ArmedCountIn || st == LayerState::ArmedOverdubCountIn)
        return;
    // Done (or cancelled during the count-in). If it recorded, the tracks
    // it printed pause -- the layer plays them now.
    if(seq_print_recorded_)
    {
        for(int k = 0; k < MidiSequencer::kTracks; k++)
            if(seq_print_tracks_ & (1u << k))
                seq_->RequestPlay(k, false);
        snprintf(seq_print_status_, sizeof(seq_print_status_), "Printed to L%d",
                  seq_print_layer_ + 1);
    }
    seq_print_layer_  = -1;
    seq_print_mask_   = 0;
    seq_print_tracks_ = 0;
}

void Ui::DrawSeqPrintPage(int t)
{
    char title[16];
    snprintf(title, sizeof(title), "M%d:Print", t + 1);
    disp_->SetCursor(0, 0);
    WriteUpper(title);
    DrawBeatIndicator(disp_->Width() - 41, 0, 3);
    disp_->DrawLine(0, 9, disp_->Width() - 1, 9, true);

    // Held B2 (before it fires): the usual hold-to-confirm bar.
    if(pod_->button2.Pressed() && !button2_long_fired_ && seq_print_layer_ < 0)
    {
        float held = pod_->button2.TimeHeldMs();
        int   w    = (int)(Clampf(held / 800.f, 0.f, 1.f) * (disp_->Width() - 2));
        disp_->SetCursor(0, 20);
        WriteUpper("Hold: Print...");
        disp_->DrawRect(0, 30, disp_->Width() - 1, 34, true, false);
        if(w > 0)
            disp_->DrawRect(1, 31, w, 33, true, true);
        DrawControlRow(kFooterRow1Y, false, kFooterDividerY, "", "", "", "");
        DrawControlRow(kFooterRow2Y, true, kFooterInterRowDividerY, "", "", "", "");
        return;
    }

    if(seq_print_dest_ >= num_layers_ && seq_print_layer_ < 0)
    {
        // Into notes: the playing pattern's arp, into the next empty one.
        char      line[40];
        const int cur    = seq_->GetPattern(t);
        int       target = -1;
        for(int p = 0; p < MidiSequencer::kPatterns && target < 0; p++)
            if(p != cur && !seq_->PatternHasNotes(t, p))
                target = p;
        snprintf(line, sizeof(line), "%c's arp into notes", 'A' + cur);
        TomThumbDrawText(disp_, 0, 17, line, true);
        if(!arp_ || !arp_->PeekPattern(t, cur).on)
            snprintf(line, sizeof(line), "(%c's arp is off)", 'A' + cur);
        else if(target < 0)
            snprintf(line, sizeof(line), "No empty pattern to print into");
        else
            snprintf(line, sizeof(line), "Into %c, its arp off", 'A' + target);
        TomThumbDrawText(disp_, 0, 26, line, true);
        TomThumbDrawText(disp_, 0, 35,
                         seq_print_status_[0] != '\0' ? seq_print_status_ : "Plays when this one ends", true);
        DrawControlRow(kFooterRow1Y, false, kFooterDividerY, "Into", "Notes", "", "");
        DrawControlRow(kFooterRow2Y, true, kFooterInterRowDividerY, SeqB1Label(t), "", "", "Hold=Print");
        return;
    }

    char     line[40];
    // Which tracks it takes.
    uint32_t tracks = SeqPrintTracks(t, nullptr);
    int      n      = snprintf(line, sizeof(line), "%s:",
                               seq_print_all_ ? "All" : MidiSequencer::EngineName(seq_->GetEngine(t)));
    for(int k = 0; k < MidiSequencer::kTracks && n < (int)sizeof(line) - 4; k++)
        if(tracks & (1u << k))
            n += snprintf(line + n, sizeof(line) - n, " M%d", k + 1);
    if(tracks == 0)
        snprintf(line + n, sizeof(line) - n, " no notes yet");
    TomThumbDrawText(disp_, 0, 17, line, true);

    const int dest = seq_print_layer_ >= 0 ? seq_print_layer_ : seq_print_dest_;
    LayerState st  = layers_[dest].GetState();
    if(seq_print_layer_ >= 0)
    {
        bool count_in = st == LayerState::ArmedCountIn || st == LayerState::ArmedOverdubCountIn;
        snprintf(line, sizeof(line), "%s into L%d...", count_in ? "Count-in" : "Printing",
                  dest + 1);
        TomThumbDrawText(disp_, 0, 26, line, true);
        // Progress through the pass.
        float p = count_in ? 0.f : layers_[dest].GetPlayPos01();
        disp_->DrawRect(0, 33, disp_->Width() - 1, 37, true, false);
        int w = (int)(Clampf(p, 0.f, 1.f) * (float)(disp_->Width() - 2));
        if(w > 0)
            disp_->DrawRect(1, 34, w, 36, true, true);
    }
    else
    {
        snprintf(line, sizeof(line), "Into L%d %s", dest + 1,
                  st == LayerState::Empty ? "(empty)" : "(overdub)");
        TomThumbDrawText(disp_, 0, 26, line, true);
        if(!SeqPrintFits(t))
            TomThumbDrawText(disp_, 0, 35, "A track is longer than the loop", true);
        else if(seq_print_status_[0] != '\0')
            TomThumbDrawText(disp_, 0, 35, seq_print_status_, true);
        else
            TomThumbDrawText(disp_, 0, 35, "Tracks pause after printing", true);
    }

    char dest_val[6];
    snprintf(dest_val, sizeof(dest_val), "L%d", dest + 1);
    DrawControlRow(kFooterRow1Y, false, kFooterDividerY, "Layer", dest_val,
                     seq_print_all_ ? "All" : "This", "Tracks");
    DrawControlRow(kFooterRow2Y, true, kFooterInterRowDividerY, SeqB1Label(t), "", "",
                     seq_print_layer_ >= 0 ? "Stop" : "Hold=Print");
}

void Ui::DrawSeqQuantPage(int t)
{
    // One bar drawn as its quantise grid: a tick per step (beats taller),
    // every second step pushed right by the swing -- where notes land.
    const int q     = seq_->GetQuantIndex(t);
    const int swing = seq_->GetSwing(t);
    const int bar_ticks = 4 * (int)MidiSequencer::kPpq;
    const int x0 = 2, x1 = disp_->Width() - 3, y_base = 36;
    disp_->DrawLine(x0, y_base, x1, y_base, true);
    if(q == 0)
        TomThumbDrawText(disp_, 0, 22, "Off -- notes play as recorded", true);
    else
    {
        const int g = (int)MidiSequencer::QuantTicks(q);
        for(int step = 0; step * g < bar_ticks; step++)
        {
            int tick = step * g + ((step & 1) ? g * swing / 200 : 0);
            int x    = x0 + tick * (x1 - x0) / bar_ticks;
            bool beat = (step * g) % (int)MidiSequencer::kPpq == 0;
            disp_->DrawLine(x, y_base - (beat ? 10 : 5), x, y_base, true);
        }
        char line[24];
        snprintf(line, sizeof(line), "Snap to %s, swing %d%%", MidiSequencer::QuantName(q),
                  swing);
        TomThumbDrawText(disp_, 0, 17, line, true);
    }

    char swing_val[8];
    snprintf(swing_val, sizeof(swing_val), "%d%%", swing);
    DrawControlRow(kFooterRow1Y, false, kFooterDividerY, "Quant", MidiSequencer::QuantName(q),
                     swing_val, "Swing");
    DrawControlRow(kFooterRow2Y, true, kFooterInterRowDividerY, SeqB1Label(t), "", "",
                     "Undo/Hold=Clr");
}

void Ui::DrawHomeSeqPage()
{
    // Home's pages 2 and 3: M1..M4 / M5..M8 in the same boxes as the
    // layers -- the same state glyphs (- empty, C count-in, R recording,
    // P playing, || paused),
    // the engine it plays, and a playhead along the bottom.
    DrawBeatIndicator(2, 0, 6);
    char line[24];
    snprintf(line, sizeof(line), "BPM:%d BARS:%d %s", (int)(tempo_->GetBpm() + 0.5f),
              tempo_->GetBars(), tempo_->IsMetronomeEnabled() ? "[M]" : "[ ]");
    TomThumbDrawText(disp_, disp_->Width() - 1 - TomThumbAdvanceWidth(line),
                     kTomThumbRowHeight - 1, line, true);
    if(!seq_)
        return;

    const int sel     = HomeSeqTrack();
    const int first   = (sel / 4) * 4;
    const int box_gap = 4;
    const int box_w   = (disp_->Width() - 3 * box_gap) / 4;
    const int box_h = 24, top = 14;
    for(int i = 0; i < 4; i++)
    {
        const int t  = first + i;
        const int x0 = i * (box_w + box_gap), x1 = x0 + box_w - 1, y1 = top + box_h - 1;
        disp_->DrawRect(x0, top, x1, y1, true, false);
        if(t == sel)
            disp_->DrawRect(x0 + 1, top + 1, x1 - 1, y1 - 1, true, false);
        StateIcon icon = StateIcon::Empty;
        switch(seq_->GetState(t))
        {
            case MidiSequencer::State::Empty: icon = StateIcon::Empty; break;
            case MidiSequencer::State::Playing: icon = StateIcon::Play; break;
            case MidiSequencer::State::Paused: icon = StateIcon::Pause; break;
            case MidiSequencer::State::Recording: icon = StateIcon::Rec; break;
            case MidiSequencer::State::Armed: icon = StateIcon::CountIn; break; // as on a layer
        }
        char label[8];
        snprintf(label, sizeof(label), "M%d", t + 1);
        disp_->SetCursor(x0 + 3, top + 4);
        WriteUpper(label);
        DrawStateIcon(x0 + 16, top + 4, icon);
        TomThumbDrawText(disp_, x0 + 3, top + 19, MidiSequencer::EngineName(seq_->GetEngine(t)),
                         true);
        // The pattern playing -- and, alternating with it, one queued next.
        {
            int  p       = seq_->GetPattern(t);
            int  next    = seq_->GetNextPattern(t);
            if(next >= 0 && (System::GetNow() / 300) % 2)
                p = next;
            char pl[2] = {(char)('A' + p), 0};
            // Clear of the selection's inner border.
            TomThumbDrawText(disp_, x1 - 5, top + 19, pl, true);
        }
        if(seq_->GetEventCount(t) > 0)
        {
            int px = x0 + 2 + (int)(seq_->GetPosition01(t) * (float)(box_w - 5));
            disp_->DrawLine(px, y1 - 2, px, y1 - 1, true);
        }
    }
    const char* what = "MIDI Tracks";
    TomThumbDrawText(disp_, (disp_->Width() - TomThumbAdvanceWidth(what)) / 2, 44, what, true);

    char vol_val[8], metro_val[8];
    snprintf(vol_val, sizeof(vol_val), "%d%%", (int)(master_volume01_ * 100.f + 0.5f));
    snprintf(metro_val, sizeof(metro_val), "%d%%",
              (int)(tempo_->GetMetronomeVolume01() * 100.f + 0.5f));
    DrawControlRow(kFooterRow1Y, false, kFooterDividerY, "Master Vol", vol_val, metro_val,
                     "Metro");
    char byp_label[12];
    snprintf(byp_label, sizeof(byp_label), "Bypass:%s", bypass_ ? "On" : "Off");
    DrawControlRow(kFooterRow2Y, true, kFooterInterRowDividerY, SeqB1Label(sel), "", "",
                     byp_label);
}

void Ui::SaveScene(int i)
{
    if(!seq_)
        return;
    Scene& s = scenes_[i];
    s        = Scene{};
    s.used   = 1;
    for(int t = 0; t < MidiSequencer::kTracks; t++)
    {
        const MidiSequencer::State st = seq_->GetState(t);
        const bool on = st == MidiSequencer::State::Playing || st == MidiSequencer::State::Recording
                        || st == MidiSequencer::State::Armed;
        // A pattern queued next is the one meant.
        const int next = seq_->GetNextPattern(t);
        s.pattern[t]   = on ? (int8_t)(next >= 0 ? next : seq_->GetPattern(t)) : -1;
        int8_t    ch[MidiSequencer::kMaxChain];
        const int n = seq_->GetChain(t, ch);
        if(on && n >= 2)
        {
            s.chain_len[t] = (uint8_t)n;
            memcpy(s.chain[t], ch, (size_t)n);
            s.pattern[t] = ch[0];
        }
    }
    for(int L = 0; L < num_layers_ && L < Scene::kLayers; L++)
    {
        const LayerState st = layers_[L].GetState();
        s.layer_on[L]    = st == LayerState::Playing || st == LayerState::Overdubbing
                        || st == LayerState::ArmedOverdubCountIn;
        s.layer_vol01[L] = layers_[L].GetVolume01();
    }
    if(scene_next_ < 0)
        scene_playing_ = i;
}

void Ui::LaunchScene(int i)
{
    if(!seq_ || !scenes_[i].used)
        return;
    if(song_playing_)
        StopSong(); // taking over by hand
    if(i == scene_next_)
    {
        // A second tap before it starts: call it off.
        seq_->CancelScene();
        for(int L = 0; L < num_layers_; L++)
            layers_[L].CancelScene();
        scene_next_ = -1;
        return;
    }
    QueueScene(i);
}

void Ui::QueueScene(int i)
{
    // kScenes: everything off (the end of a song); an empty scene is the same.
    static const Scene kAllOff{};
    const Scene&       s = i >= 0 && i < kScenes && scenes_[i].used ? scenes_[i] : kAllOff;
    seq_->RequestScene(s.pattern, s.chain, s.chain_len);
    // With the loops switched off nothing reaches a loop start: now.
    const bool now = !IsLooperEnabled();
    for(int L = 0; L < num_layers_ && L < Scene::kLayers; L++)
    {
        // Everything off pauses the layers but leaves their volumes be.
        const float vol = &s == &kAllOff ? -1.f : s.layer_vol01[L];
        if(now)
            layers_[L].ApplySceneNow(s.layer_on[L] != 0, vol);
        else
            layers_[L].QueueScene(s.layer_on[L] != 0, vol);
    }
    scene_next_ = i;
}

void Ui::UpdateScenes()
{
    // A launched scene starts with the loop: it's playing once the
    // sequencer has taken it.
    if(scene_next_ < 0 || !seq_ || seq_->ScenePending())
        return;
    scene_playing_ = scene_next_ < kScenes ? scene_next_ : -1;
    scene_next_    = -1;
    if(song_rec_ && !song_rec_armed_ && scene_playing_ >= 0)
    {
        // Recording a song: the step before ends here, this one begins.
        const uint32_t loops = g_loop_starts - song_rec_loop_;
        if(song_.len > 0 && loops == 0)
            song_.steps[song_.len - 1].scene = (uint8_t)scene_playing_; // replaced before it played
        else if(song_.len < Song::kSteps)
        {
            if(song_.len > 0)
                song_.steps[song_.len - 1].repeats
                    = (uint8_t)(loops > (uint32_t)Song::kMaxRepeats ? Song::kMaxRepeats : loops);
            song_.steps[song_.len].scene   = (uint8_t)scene_playing_;
            song_.steps[song_.len].repeats = 1;
            song_.len++;
        }
        song_rec_loop_ = g_loop_starts;
    }
}

void Ui::StartSong(int from)
{
    if(!seq_ || song_.len == 0 || !IsLooperEnabled())
        return;
    if(song_rec_)
        StopSongRecord();
    song_step_    = from >= 0 && from < song_.len ? from : 0;
    song_playing_ = true;
    song_waiting_ = true;
    song_queued_  = false;
    QueueScene(song_.steps[song_step_].scene);
}

void Ui::StopSong()
{
    // The arrangement stops; whatever scene is playing carries on.
    song_playing_ = false;
    if(scene_next_ >= 0 && seq_)
    {
        seq_->CancelScene();
        for(int L = 0; L < num_layers_; L++)
            layers_[L].CancelScene();
        scene_next_ = -1;
    }
}

void Ui::StartSongRecord()
{
    if(song_playing_)
        StopSong();
    song_.len       = 0;
    song_cursor_    = 0;
    song_rec_       = true;
    song_rec_armed_ = true; // starts with the loop, on the scene playing then
    song_rec_loop_  = g_loop_starts;
}

void Ui::StopSongRecord()
{
    if(!song_rec_)
        return;
    song_rec_ = false;
    if(song_rec_armed_)
    {
        song_rec_armed_ = false;
        return;
    }
    if(song_.len == 0)
        return;
    // The last step's loops -- the one under way counts once it's past half.
    uint32_t loops = g_loop_starts - song_rec_loop_;
    const uint32_t loop_ticks
        = (uint32_t)tempo_->GetBars() * TempoClock::kBeatsPerBar * MidiSequencer::kPpq;
    if(loop_ticks > 0 && tempo_->GetLoopTick(MidiSequencer::kPpq) * 2 >= loop_ticks)
        loops++;
    if(loops < 1)
        loops = 1;
    song_.steps[song_.len - 1].repeats
        = (uint8_t)(loops > (uint32_t)Song::kMaxRepeats ? Song::kMaxRepeats : loops);
}

void Ui::UpdateSong()
{
    if(song_rec_armed_ && g_loop_starts != song_rec_loop_)
    {
        // Recording begins with this loop, on the scene playing.
        song_rec_armed_ = false;
        song_rec_loop_  = g_loop_starts;
        if(scene_playing_ >= 0 && scene_next_ < 0)
        {
            song_.steps[0].scene   = (uint8_t)scene_playing_;
            song_.steps[0].repeats = 1;
            song_.len              = 1;
        }
    }
    if(!song_playing_)
        return;
    if(!IsLooperEnabled() || song_step_ >= song_.len)
    {
        StopSong();
        return;
    }
    if(song_waiting_)
    {
        if(scene_next_ >= 0)
            return; // its first scene hasn't started yet
        song_waiting_   = false;
        song_queued_    = false;
        song_step_loop_ = g_loop_starts;
    }
    if(g_loop_starts - song_step_loop_ >= song_.steps[song_step_].repeats)
    {
        // The next step's scene took over at this loop's start.
        song_step_loop_ += song_.steps[song_step_].repeats;
        song_step_++;
        song_queued_ = false;
        if(song_step_ >= song_.len)
        {
            song_playing_ = false; // the end: everything went off with it
            return;
        }
    }
    // In a step's last loop, launch what follows (everything off after the
    // last), so it switches as the loop starts over.
    if(!song_queued_ && g_loop_starts - song_step_loop_ + 1 >= song_.steps[song_step_].repeats)
    {
        song_queued_ = true;
        QueueScene(song_step_ + 1 < song_.len ? song_.steps[song_step_ + 1].scene : kScenes);
    }
}

void Ui::DrawHomeSongPage()
{
    char title[24];
    if(song_rec_)
        snprintf(title, sizeof(title), "Song:Rec%s", song_rec_armed_ ? "..." : "");
    else if(song_playing_ && !song_waiting_ && song_step_ < song_.len)
        snprintf(title, sizeof(title), "Song %d %d/%d", song_step_ + 1,
                 (int)(g_loop_starts - song_step_loop_) + 1, song_.steps[song_step_].repeats);
    else
        snprintf(title, sizeof(title), song_edit_ ? "Song:Edit" : "Song");
    disp_->SetCursor(0, 0);
    WriteUpper(title);
    DrawBeatIndicator(disp_->Width() - 41, 0, 3);
    disp_->DrawLine(0, 9, disp_->Width() - 1, 9, true);

    if(pod_->button2.Pressed() && pod_->button2.TimeHeldMs() > 200.f && !button2_long_fired_
       && song_cursor_ < song_.len)
    {
        float held = pod_->button2.TimeHeldMs();
        int   w    = (int)(Clampf(held / 800.f, 0.f, 1.f) * (disp_->Width() - 2));
        char  msg[24];
        snprintf(msg, sizeof(msg), "Hold: Delete %d...", song_cursor_ + 1);
        disp_->SetCursor(0, 20);
        WriteUpper(msg);
        disp_->DrawRect(0, 30, disp_->Width() - 1, 34, true, false);
        if(w > 0)
            disp_->DrawRect(1, 31, w, 33, true, true);
        DrawControlRow(kFooterRow1Y, false, kFooterDividerY, "", "", "", "");
        DrawControlRow(kFooterRow2Y, true, kFooterInterRowDividerY, "", "", "", "");
        return;
    }

    // Three columns of four, read down then across, with lines between:
    // step, scene, repeats. The cursor's step is inverted, > marks the one
    // playing, and the slot past the last adds one. The view follows the
    // step playing, unless K1 moved the cursor in the last few seconds
    // (or Edit is on).
    constexpr int kRows = 4, kCols = 3, kColW = 43;
    const int  total   = song_.len + 1;
    const bool follow  = song_playing_ && !song_waiting_ && !song_edit_
                        && System::GetNow() - song_k1_ms_ > 3000;
    const int  target  = follow ? song_step_ : song_cursor_;
    const int  tcol    = target / kRows;
    if(tcol < song_view_col_)
        song_view_col_ = tcol;
    if(tcol > song_view_col_ + kCols - 1)
        song_view_col_ = tcol - (kCols - 1);
    const int last_col = (total - 1) / kRows;
    if(song_view_col_ > last_col)
        song_view_col_ = last_col;
    if(song_view_col_ < 0)
        song_view_col_ = 0;
    for(int c = 1; c < kCols; c++)
        disp_->DrawLine(c * kColW - 1, 11, c * kColW - 1, 11 + kRows * 7 - 4, true);
    for(int k = 0; k < kRows * kCols; k++)
    {
        const int i = song_view_col_ * kRows + k;
        if(i >= total)
            break;
        const int  x0 = (k / kRows) * kColW, y0 = 11 + (k % kRows) * 7, base = y0 + 6;
        const bool sel = i == song_cursor_;
        if(sel)
            disp_->DrawRect(x0, y0, x0 + kColW - 2, y0 + 6, true, true);
        if(i < song_.len)
        {
            char n[4], sc[4], rp[4];
            snprintf(n, sizeof(n), "%d", i + 1);
            snprintf(sc, sizeof(sc), "S%d", song_.steps[i].scene + 1);
            snprintf(rp, sizeof(rp), "x%d", song_.steps[i].repeats);
            if(song_playing_ && i == song_step_)
                TomThumbDrawText(disp_, x0 + 1, base, ">", !sel);
            // Each digit in its own 4px cell -- the narrower "1" would
            // otherwise sit a pixel off the column.
            const int nl = (int)strlen(n);
            for(int d = 0; d < nl; d++)
                TomThumbDrawChar(disp_, x0 + 13 - 4 * (nl - d), base, n[d], !sel);
            TomThumbDrawText(disp_, x0 + 16, base, sc, !sel);
            TomThumbDrawText(disp_, x0 + 30, base, rp, !sel);
        }
        else
            TomThumbDrawText(disp_, x0 + 5, base, song_.len < Song::kSteps ? "+ add" : "full",
                             !sel);
    }
    const char* hint = song_edit_ ? "CLICK ENC TO FINISH" : "CLICK ENC TO EDIT";
    TomThumbDrawText(disp_, (disp_->Width() - TomThumbAdvanceWidth(hint)) / 2, 44, hint, true);

    if(song_edit_)
    {
        char sc[4] = "-", rp[6] = "-";
        if(song_cursor_ < song_.len)
        {
            snprintf(sc, sizeof(sc), "%d", song_.steps[song_cursor_].scene + 1);
            snprintf(rp, sizeof(rp), "x%d", song_.steps[song_cursor_].repeats);
        }
        DrawControlRow(kFooterRow1Y, false, kFooterDividerY, "Scene", sc, rp, "Repeats");
    }
    else
    {
        char st[8], vol_val[8];
        snprintf(st, sizeof(st), "%d/%d", song_cursor_ + 1, song_.len);
        snprintf(vol_val, sizeof(vol_val), "%d%%", (int)(master_volume01_ * 100.f + 0.5f));
        DrawControlRow(kFooterRow1Y, false, kFooterDividerY, "Step", st, vol_val, "Master Vol");
    }
    DrawControlRow(kFooterRow2Y, true, kFooterInterRowDividerY,
                   song_rec_ ? "Stop rec" : song_playing_ ? "Stop" : "Play/Hold=Rec", "", "",
                   song_cursor_ < song_.len ? "Ins/Hold=Del" : "Add");
}

void Ui::SceneEditValue(float k2)
{
    Scene&     s    = scenes_[scene_edit_];
    const int  c    = scene_edit_cell_;
    const bool live = scene_playing_ == scene_edit_ && scene_next_ < 0;
    s.used          = 1;
    if(c < Scene::kLayers)
    {
        s.layer_vol01[c] = Clampf(k2, 0.f, 1.f);
        if(live && c < num_layers_)
            layers_[c].SetVolume01(s.layer_vol01[c]);
        return;
    }
    const int t = c - Scene::kLayers;
    const int p = (int)(Clampf(k2, 0.f, 0.999f) * 9.f) - 1; // Off, A..H
    if(p == s.pattern[t] && s.chain_len[t] < 2)
        return;
    s.pattern[t]   = (int8_t)p;
    s.chain_len[t] = 0; // a pattern of its own replaces a chain
    if(live && seq_)
    {
        if(p < 0)
            seq_->RequestPlay(t, false);
        else
        {
            seq_->RequestPattern(t, p);
            seq_->RequestPlay(t, true);
        }
    }
}

void Ui::SceneEditToggle()
{
    Scene&     s    = scenes_[scene_edit_];
    const int  c    = scene_edit_cell_;
    const bool live = scene_playing_ == scene_edit_ && scene_next_ < 0;
    s.used          = 1;
    if(c < Scene::kLayers)
    {
        s.layer_on[c] = !s.layer_on[c];
        if(live && c < num_layers_)
            layers_[c].ApplySceneNow(s.layer_on[c] != 0, -1.f);
        return;
    }
    const int t = c - Scene::kLayers;
    if(s.pattern[t] >= 0)
    {
        s.pattern[t]   = -1;
        s.chain_len[t] = 0;
        if(live && seq_)
            seq_->RequestPlay(t, false);
    }
    else
    {
        // Back on with the track's own pattern.
        const int p  = seq_ ? seq_->GetPattern(t) : 0;
        s.pattern[t] = (int8_t)p;
        if(live && seq_)
            seq_->RequestPlay(t, true);
    }
    SyncPickupTargets(KnobContext::HomeSceneEdit);
}

void Ui::DrawSceneEditPage()
{
    const Scene& s = scenes_[scene_edit_];
    char         title[16];
    snprintf(title, sizeof(title), "Scene %d:Edit", scene_edit_ + 1);
    disp_->SetCursor(0, 0);
    WriteUpper(title);
    DrawBeatIndicator(disp_->Width() - 41, 0, 3);
    disp_->DrawLine(0, 9, disp_->Width() - 1, 9, true);

    // Three columns of four: the layers, M1..M4, M5..M8. The chosen one
    // is inverted; a chain shows as its first pattern and a +.
    constexpr int kRows = 4, kColW = 43;
    for(int c = 1; c < 3; c++)
        disp_->DrawLine(c * kColW - 1, 11, c * kColW - 1, kFooterDividerY - 1, true);
    char cur_val[8] = "", cur_name[4] = "";
    for(int c = 0; c < kSceneCells; c++)
    {
        const int  x0 = (c / kRows) * kColW, y0 = 12 + (c % kRows) * 8, base = y0 + 6;
        const bool sel = c == scene_edit_cell_;
        char       name[4], val[8];
        if(c < Scene::kLayers)
        {
            snprintf(name, sizeof(name), "L%d", c + 1);
            if(s.layer_on[c])
                snprintf(val, sizeof(val), "%d%%", (int)(s.layer_vol01[c] * 100.f + 0.5f));
            else
                snprintf(val, sizeof(val), "off");
        }
        else
        {
            const int t = c - Scene::kLayers;
            snprintf(name, sizeof(name), "M%d", t + 1);
            if(s.pattern[t] < 0)
                snprintf(val, sizeof(val), "off");
            else
                snprintf(val, sizeof(val), "%c%s", 'A' + s.pattern[t], s.chain_len[t] >= 2 ? "+" : "");
        }
        if(sel)
        {
            disp_->DrawRect(x0, y0, x0 + kColW - 2, y0 + 7, true, true);
            snprintf(cur_name, sizeof(cur_name), "%s", name);
            if(c < Scene::kLayers)
                snprintf(cur_val, sizeof(cur_val), "%d%%", (int)(s.layer_vol01[c] * 100.f + 0.5f));
            else
                snprintf(cur_val, sizeof(cur_val), "%s", val);
        }
        TomThumbDrawText(disp_, x0 + 2, base, name, !sel);
        TomThumbDrawText(disp_, x0 + 16, base, val, !sel);
    }
    DrawControlRow(kFooterRow1Y, false, kFooterDividerY, "Pick", cur_name, cur_val,
                   scene_edit_cell_ < Scene::kLayers ? "Volume" : "Pattern");
    DrawControlRow(kFooterRow2Y, true, kFooterInterRowDividerY, "Play/Hold=Save", "", "",
                   "On/Off");
}

void Ui::DrawHomeScenePage()
{
    if(scene_edit_ >= 0)
    {
        DrawSceneEditPage();
        return;
    }
    disp_->SetCursor(0, 0);
    WriteUpper("Scenes");
    DrawBeatIndicator(disp_->Width() - 41, 0, 3);
    disp_->DrawLine(0, 9, disp_->Width() - 1, 9, true);

    // Held B2 (past a tap, before it fires): the usual hold-to-confirm bar.
    if(pod_->button2.Pressed() && pod_->button2.TimeHeldMs() > 200.f && !button2_long_fired_)
    {
        float held = pod_->button2.TimeHeldMs();
        int   w    = (int)(Clampf(held / 800.f, 0.f, 1.f) * (disp_->Width() - 2));
        char  msg[24];
        snprintf(msg, sizeof(msg), "Hold: Clear %d...", scene_cursor_ + 1);
        disp_->SetCursor(0, 20);
        WriteUpper(msg);
        disp_->DrawRect(0, 30, disp_->Width() - 1, 34, true, false);
        if(w > 0)
            disp_->DrawRect(1, 31, w, 33, true, true);
        DrawControlRow(kFooterRow1Y, false, kFooterDividerY, "", "", "", "");
        DrawControlRow(kFooterRow2Y, true, kFooterInterRowDividerY, "", "", "", "");
        return;
    }

    // Two rows of eight: filled = playing, outlined = saved, dotted =
    // empty; the launched one blinks, the K1 choice has a mark under it.
    const bool blink = (System::GetNow() / 300) % 2;
    for(int i = 0; i < kScenes; i++)
    {
        const int  x0 = 1 + (i % 8) * 16, x1 = x0 + 13;
        const int  y0 = 12 + (i / 8) * 16, y1 = y0 + 11;
        const bool fill = i == scene_playing_ || (i == scene_next_ && blink);
        if(fill)
            disp_->DrawRect(x0, y0, x1, y1, true, true);
        else if(scenes_[i].used)
            disp_->DrawRect(x0, y0, x1, y1, true, false);
        else
            for(int x = x0; x <= x1; x += 2)
            {
                disp_->DrawPixel(x, y0, true);
                disp_->DrawPixel(x, y1, true);
            }
        char num[4];
        snprintf(num, sizeof(num), "%d", i + 1);
        TomThumbDrawText(disp_, x0 + (14 - TomThumbAdvanceWidth(num)) / 2 + 1, y0 + 8, num,
                         !fill);
        if(i == scene_cursor_)
            disp_->DrawRect(x0, y1 + 2, x1, y1 + 3, true, true);
    }

    char cur[4], vol_val[8];
    snprintf(cur, sizeof(cur), "%d", scene_cursor_ + 1);
    snprintf(vol_val, sizeof(vol_val), "%d%%", (int)(master_volume01_ * 100.f + 0.5f));
    DrawControlRow(kFooterRow1Y, false, kFooterDividerY, "Scene", cur, vol_val, "Master Vol");
    DrawControlRow(kFooterRow2Y, true, kFooterInterRowDividerY,
                   scenes_[scene_cursor_].used ? "Play/Hold=Save" : "Hold=Save", "", "",
                   scenes_[scene_cursor_].used ? "Hold=Clear" : "");
}

void Ui::DrawSeqTrackScreen()
{
    const int t = HomeSeqTrack();
    if(seq_ && t >= 0 && seq_page_ >= kSeqPageGrid)
    {
        DrawSeqGridPage(t);
        return;
    }
    if(seq_ && t >= 0 && seq_page_ == kSeqPagePatt)
    {
        DrawSeqPattPage(t);
        return;
    }
    if(seq_ && t >= 0 && seq_page_ == kSeqPageChain)
    {
        DrawSeqChainPage(t);
        return;
    }
    if(seq_ && t >= 0 && seq_page_ == kSeqPageArp)
    {
        DrawSeqArpPage(t);
        return;
    }
    if(seq_ && t >= 0 && seq_page_ == kSeqPagePrint)
    {
        DrawSeqPrintPage(t);
        return;
    }
    char      title[16];
    snprintf(title, sizeof(title), "M%d:%s", t + 1,
              seq_page_ == kSeqPageQuant ? "Quant" : "Track");
    disp_->SetCursor(0, 0);
    WriteUpper(title);
    DrawBeatIndicator(disp_->Width() - 41, 0, 3);
    disp_->DrawLine(0, 9, disp_->Width() - 1, 9, true);
    if(!seq_ || t < 0)
        return;

    // Held B2: the same hold-to-confirm bar as every other clear.
    if(pod_->button2.Pressed() && !button2_long_fired_)
    {
        float held = pod_->button2.TimeHeldMs();
        int   w    = (int)(Clampf(held / 800.f, 0.f, 1.f) * (disp_->Width() - 2));
        disp_->SetCursor(0, 20);
        WriteUpper("Hold: Clear...");
        disp_->DrawRect(0, 30, disp_->Width() - 1, 34, true, false);
        if(w > 0)
            disp_->DrawRect(1, 31, w, 33, true, true);
        DrawControlRow(kFooterRow1Y, false, kFooterDividerY, "", "", "", "");
        DrawControlRow(kFooterRow2Y, true, kFooterInterRowDividerY, "", "", "", "");
        return;
    }

    if(seq_page_ == kSeqPageQuant)
    {
        DrawSeqQuantPage(t);
        return;
    }

    const char* state = "Empty";
    switch(seq_->GetState(t))
    {
        case MidiSequencer::State::Empty: state = "Empty"; break;
        case MidiSequencer::State::Playing: state = "Playing"; break;
        case MidiSequencer::State::Paused: state = "Paused"; break;
        case MidiSequencer::State::Armed: state = "Count-in"; break;
        case MidiSequencer::State::Recording: state = "Recording"; break;
    }
    const int li = seq_->GetLengthIndex(t);
    char      line[32];
    snprintf(line, sizeof(line), "%s  %d notes", state, seq_->GetEventCount(t) / 2);
    TomThumbDrawText(disp_, 0, 17, line, true);
    // The track's own length in bars; shorter than one loop reads as a
    // fraction of it.
    if(li >= MidiSequencer::kDefaultLength)
        snprintf(line, sizeof(line), "Length %s = %d bars", MidiSequencer::LengthName(li),
                  tempo_->GetBars() << (li - MidiSequencer::kDefaultLength));
    else
        snprintf(line, sizeof(line), "Length %s loop", MidiSequencer::LengthName(li));
    TomThumbDrawText(disp_, 0, 25, line, true);
    TomThumbDrawText(disp_, 0, 33, "Keys play this track's engine", true);

    // Playhead across the track's length.
    disp_->DrawRect(0, 37, disp_->Width() - 1, 41, true, false);
    int fill = (int)(seq_->GetPosition01(t) * (float)(disp_->Width() - 2));
    if(fill > 0 && seq_->GetEventCount(t) > 0)
        disp_->DrawRect(1, 38, fill, 40, true, true);

    DrawControlRow(kFooterRow1Y, false, kFooterDividerY, "Engine",
                     MidiSequencer::EngineName(seq_->GetEngine(t)), MidiSequencer::LengthName(li),
                     "Length");
    DrawControlRow(kFooterRow2Y, true, kFooterInterRowDividerY, SeqB1Label(t), "", "",
                     "Undo/Hold=Clr");
}

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
void Ui::DrawGranularWaveformLive(float range_start01, float range_end01)
{
    float pointers[4];
    int   n = granular_->GetVoicePointers01(pointers, 4);
    DrawGranularWaveform(granular_->GetWaveformPeaks(), granular_->GetPosition01(), pointers, n,
                         granular_->HasSource(), granular_->GetGrainSizeFraction01(),
                         range_start01, range_end01);
}

void Ui::DrawGranularWaveform(const float* peaks, float grain_anchor01, const float* pointers01,
                                int num_pointers, bool has_source, float grain_size01,
                                float range_start01, float range_end01)
{
    float max_peak = 0.f;
    for(int col = 0; col < GranularEngine::kWaveformCols; col++)
        if(peaks[col] > max_peak)
            max_peak = peaks[col];
    float scale = max_peak > 0.001f ? 1.f / max_peak : 0.f;

    // kBandTop pushed down to 27 (vs DrawWaveform()'s 14) -- this page's
    // header is now two Tom Thumb rows (label + centered value) instead
    // of one, ending around y21, so this needs the extra clearance.
    const int kBandTop    = 28; // the markers above it (from kBandTop - 4) clear the text
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
        // Each sounding voice's pointer: a tick one row above Position's
        // span, so they stay distinct even sitting on the same column
        // (every note starts its pointer at Position).
        for(int k = 0; k < num_pointers; k++)
        {
            int col_s = (int)(Clampf(pointers01[k], 0.f, 1.f) * GranularEngine::kWaveformCols);
            if(col_s >= GranularEngine::kWaveformCols)
                col_s = GranularEngine::kWaveformCols - 1;
            int px_s = 1 + col_s * 2;
            disp_->DrawLine(px_s, kBandTop - 4, px_s, kBandTop - 3, true);
        }

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
        case LayerPage::Filter: page_name = "Filter"; break;
        case LayerPage::Drive: page_name = "Drive"; break;
        case LayerPage::Tremolo: page_name = "Tremolo"; break;
        case LayerPage::Reverb: page_name = "LYR-FX"; break;
        case LayerPage::Gain: page_name = "Gain"; break;
        default: break;
    }
    // Compact "N:Page" (not "Layer N - Page") specifically to leave room
    // for the beat indicator on the same row -- see DrawBeatIndicator().
    snprintf(title, sizeof(title), "A%d:%s", cursor_layer_ + 1, page_name);
    disp_->SetCursor(0, 0);
    WriteUpper(title);
    DrawBeatIndicator(disp_->Width() - 41, 0, 3);
    disp_->DrawLine(0, 9, disp_->Width() - 1, 9, true);

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
                case LayerState::Empty: rec_label = "Hold=Rec"; break;
                case LayerState::ArmedCountIn: rec_label = "Cancel"; break;
                case LayerState::Recording: rec_label = "Stop"; break;
                // "ShortPressFn/LongPressFn" order, consistently -- see
                // the Button2 label right below for the matching
                // "Undo/Clear" convention on this same row.
                case LayerState::Playing: rec_label = "Pause/Hold=Rec"; break;
                case LayerState::Paused: rec_label = "Play/Hold=Rec"; break;
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
        case LayerPage::Filter:
        {
            DrawFilterCurve(Cur().GetFilterMode(), Cur().GetFilterCutoff01(),
                            Cur().GetFilterResonance01());

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
        case LayerPage::Drive:
        {
            DrawDriveCurve(Cur().GetDrive01(), Cur().GetDriveTrim01());
            char drive_val[8], trim_val[8];
            snprintf(drive_val, sizeof(drive_val), "%d%%", (int)(Cur().GetDrive01() * 100.f + 0.5f));
            snprintf(trim_val, sizeof(trim_val), "%d%%", (int)(Cur().GetDriveTrim01() * 100.f + 0.5f));
            DrawControlRow(kFooterRow1Y, false, kFooterDividerY, "Drive", drive_val, trim_val, "Trim");
            DrawControlRow(kFooterRow2Y, true, kFooterInterRowDividerY, "", "", "", "");
            break;
        }
        case LayerPage::Tremolo:
        {
            DrawTremoloCurve(Cur().GetTremoloDepth01(), Cur().GetTremoloRate01());
            // See the Gain page on why not %f.
            const int hz_x10 = (int)(LooperLayer::TremoloHz(Cur().GetTremoloRate01()) * 10.f + 0.5f);
            char depth_val[8], rate_val[10];
            snprintf(depth_val, sizeof(depth_val), "%d%%",
                     (int)(Cur().GetTremoloDepth01() * 100.f + 0.5f));
            snprintf(rate_val, sizeof(rate_val), "%d.%dHz", hz_x10 / 10, hz_x10 % 10);
            DrawControlRow(kFooterRow1Y, false, kFooterDividerY, "Depth", depth_val, rate_val, "Rate");
            DrawControlRow(kFooterRow2Y, true, kFooterInterRowDividerY, "", "", "", "");
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
            DrawFxSends(Cur().GetReverbSend01(), Cur().GetDelaySend01(), -1);
            DrawControlRow(kFooterRow1Y, false, kFooterDividerY, "Reverb", rev_val, delay_val,
                             "Delay");
            DrawControlRow(kFooterRow2Y, true, kFooterInterRowDividerY, "", "", "", "");
            break;
        }
        case LayerPage::Gain:
        {
            // %f is used nowhere in this file on purpose: this build links
            // --specs=nano.specs, whose snprintf prints nothing for float
            // conversions -- integer tenths instead.
            int gain_x10 = (int)(Cur().GetInputGain() * 10.f + 0.5f);
            // While this page is open the input is monitored through
            // this gain (see IsBypassed()), and this is it, live.
            DrawGainScope();

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
    if(global_page_ == GlobalPage::Snap)
    {
        DrawSnapScreen();
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
    if(global_page_ == GlobalPage::Midi)
    {
        DrawMidiScreen();
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
        DrawGlobalEntryPage("Global:Grains", granular_enabled_, kGlobalIconGrains);
        return;
    }
    if(global_page_ == GlobalPage::Display)
    {
        DrawDisplayPage();
        return;
    }
    if(global_page_ == GlobalPage::Looper)
    {
        // Click opens Home (the layer boxes), the Looper's own screen --
        // that's just the "any other Global page goes to Home" click
        // rule, so it needs no case of its own. See IsLooperEnabled()'s
        // own comment for the pause-vs-full-stop distinction.
        DrawGlobalEntryPage("Global:Looper", looper_enabled_, kGlobalIconLooper,
                             looper_status_[0] != '\0'
                                 ? looper_status_
                                 : (looper_default_on_ ? "DEFAULT:ON" : "DEFAULT:OFF"),
                             "Default");
        return;
    }
    if(global_page_ == GlobalPage::Sends)
    {
        DrawSendsPage();
        return;
    }
    if(global_page_ == GlobalPage::Arp)
    {
        DrawArpGlobalPage();
        return;
    }
    if(global_page_ == GlobalPage::MixTracks || global_page_ == GlobalPage::MixSynths)
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
        WriteUpper(global_page_ == GlobalPage::MixSynths ? "Global:Synths" : "Global:Tracks");
        DrawBeatIndicator(disp_->Width() - 41, 0, 3);
        disp_->DrawLine(0, 9, disp_->Width() - 1, 9, true);

        if(mixer_profile_view_)
        {
            // Per-stage CPU, avg/peak % of the block budget (profiler.h),
            // three columns of four under an AVG/PK heading. The average
            // ends 1px before a fixed "/" and the peak starts 1px after
            // it, so the slash always sits midway between the two; the
            // headings are centred over two-digit numbers.
            static const char* const kNames[Prof::kNumStages] = {
                "Gran", "DXD", "Ens", "Orgn", "SmVo", "SmDr", "SSaw", "Loop", "Dly", "Rev", "Othr"};
            constexpr int kColW = 43, kAvgRight = 24, kSlashX = 26, kPeakLeft = 30, kTwoDigitW = 7;
            for(int col = 0; col < 3; col++)
            {
                const int avg_mid  = col * kColW + kAvgRight - (kTwoDigitW - 1) / 2;
                const int peak_mid = col * kColW + kPeakLeft + (kTwoDigitW - 1) / 2;
                TomThumbDrawText(disp_, avg_mid - (TomThumbInkWidth("AVG") - 1) / 2, 16, "AVG",
                                 true);
                TomThumbDrawText(disp_, peak_mid - (TomThumbInkWidth("PK") - 1) / 2, 16, "PK",
                                 true);
            }
            for(int st = 0; st < Prof::kNumStages; st++)
            {
                // y is the text baseline (glyphs sit above it): 21..39
                // sits under the heading and clear of the footer (row 46).
                const int x = (st % 3) * kColW, y = 21 + (st / 3) * 6;
                char      avg[6], peak[6];
                snprintf(avg, sizeof(avg), "%d", (int)(Prof::g_avg[st] * 100.f + 0.5f));
                snprintf(peak, sizeof(peak), "%d", (int)(Prof::g_peak[st] * 100.f + 0.5f));
                TomThumbDrawText(disp_, x, y, kNames[st], true);
                TomThumbDrawText(disp_, x + kAvgRight - TomThumbInkWidth(avg) + 1, y, avg, true);
                TomThumbDrawText(disp_, x + kSlashX, y, "/", true);
                TomThumbDrawText(disp_, x + kPeakLeft, y, peak, true);
            }
        }
        else
        {
            mixer_set_ = global_page_ == GlobalPage::MixSynths ? 1 : 0;
            DrawMixerOverviewGrid(16);
            const char* hint = "PUSH ENC TO OPEN";
            int         hw   = TomThumbAdvanceWidth(hint);
            TomThumbDrawText(disp_, (disp_->Width() - hw) / 2, 44, hint, true);
        }

        // K1 Master Vol / K2 Bypass Vol, and the live CPU load on the
        // bottom row (no buttons on this page, so that row is free -- no
        // centre icon, since it's a readout, not a control). Avg is a
        // continuously-smoothed reading (see daisy::CpuLoadMeter's own
        // 1-pole filter); Peak rolls over every ~10s (see main.cpp's
        // kCpuPeakResetSeconds) so it reads fresh again shortly after you
        // change something.
        {
            char mvol_val[8], bvol_val[8];
            snprintf(mvol_val, sizeof(mvol_val), "%d%%", (int)(master_volume01_ * 100.f + 0.5f));
            snprintf(bvol_val, sizeof(bvol_val), "%d%%",
                      (int)(bypass_mix_volume01_ * 100.f + 0.5f));
            DrawControlRow(kFooterRow1Y, false, kFooterDividerY, "Master Vol", mvol_val, bvol_val,
                             "Bypass Vol");
            char avg_val[8], peak_val[8];
            snprintf(avg_val, sizeof(avg_val), "%d%%", (int)(g_cpu_avg_load * 100.f + 0.5f));
            snprintf(peak_val, sizeof(peak_val), "%d%%", (int)(g_cpu_peak_load * 100.f + 0.5f));
            DrawControlRow(kFooterRow2Y, false, kFooterInterRowDividerY, "CPU", avg_val, peak_val,
                             "Peak", /*show_icon=*/false);
        }
        return;
    }
    if(global_page_ == GlobalPage::Dexed)
    {
        // Entry point into Screen::Dexed, plus the on/off toggle -- same
        // treatment as Global:Granular above.
        char voices[12];
        snprintf(voices, sizeof(voices), "VOICES %d", dexed_engines_[0]->GetMaxVoices());
        DrawGlobalEntryPage("Global:DXD-A", dexed_enabled_, kGlobalIconDexedA, voices);
        return;
    }
    if(global_page_ == GlobalPage::DexedB)
    {
        char voices[12];
        snprintf(voices, sizeof(voices), "VOICES %d", dexed_engines_[1]->GetMaxVoices());
        DrawGlobalEntryPage("Global:DXD-B", dexed_b_enabled_, kGlobalIconDexedB, voices);
        return;
    }
    if(global_page_ == GlobalPage::Ensemble)
    {
        // Entry point into Screen::Ensemble, plus the on/off toggle --
        // same treatment as Global:Granular/Global:Dexed above.
        // "Ensmbl" not "Ensemble" -- same budget as Global:Grains'
        // own shortened label (title row reserves the right 41px for
        // the beat indicator, ~14 chars max in Font_6x8 before it
        // overlaps -- "Global:Ensemble" didn't fit, confirmed on real
        // hardware).
        DrawGlobalEntryPage("Global:Ensmbl", ensemble_enabled_, kGlobalIconEnsemble);
        return;
    }
    if(global_page_ == GlobalPage::Organ)
    {
        DrawGlobalEntryPage("Global:Organ", organ_enabled_, kGlobalIconOrgan);
        return;
    }
    if(global_page_ == GlobalPage::Supersaw)
    {
        DrawGlobalEntryPage("Global:SSaw", supersaw_enabled_, kGlobalIconSupersaw);
        return;
    }
    if(global_page_ == GlobalPage::SampleSynthVoice)
    {
        // Entry point into Screen::SampleSynth (Voice face), plus the
        // on/off toggle -- same treatment as Global:Granular/Dexed/
        // Ensemble above, but mutually exclusive with SMPDR below (see
        // sample_synth_voice_enabled_'s own comment).
        DrawGlobalEntryPage("Global:SMPVO", sample_synth_voice_enabled_, kGlobalIconSmpvo);
        return;
    }
    if(global_page_ == GlobalPage::SampleSynthDrum)
    {
        DrawGlobalEntryPage("Global:SMPDR", sample_synth_drum_enabled_, kGlobalIconSmpdr);
        return;
    }

    // Compact form (not "Global Settings") for the same reason as the
    // Layer screen's title -- leaves room for the beat indicator on the
    // same row.
    const char* title = "Global:Tempo";
    if(global_page_ == GlobalPage::Filter)
        title = "Global:Filter";
    else if(global_page_ == GlobalPage::Reverb)
        title = "Global:Reverb";
    else if(global_page_ == GlobalPage::Delay)
        title = "Global:Delay";
    else if(global_page_ == GlobalPage::DelayReverb)
        title = "Global:Dly-Rev";
    else if(global_page_ == GlobalPage::DelayTone)
        title = "Global:DlyTone";
    else if(global_page_ == GlobalPage::Bypass)
        title = "Global:Bypass";
    else if(global_page_ == GlobalPage::ReverbTone)
        title = "Global:RevTone";
    else if(global_page_ == GlobalPage::ReverbDiffusion)
        title = "Global:RevDiff";
    disp_->SetCursor(0, 0);
    WriteUpper(title);
    DrawBeatIndicator(disp_->Width() - 41, 0, 3);
    disp_->DrawLine(0, 9, disp_->Width() - 1, 9, true);

    char line1[32];
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

        // A metronome (pendulum swinging beat to beat while Metro is on,
        // upright while it's off) with On/Off under it; big BPM; then a
        // pad per beat of the bar and a pad per bar up to the BPM's own
        // ceiling (see TempoClock::MaxBarsForBpm()) -- the loop's bars
        // outlined, the current one filled, the rest dotted.
        const bool metro_on = tempo_->IsMetronomeEnabled();
        const int  beat     = tempo_->GetBeatInBar();
        DrawMetronome(4, 12, !metro_on ? 0 : (beat % 2 == 0 ? -1 : 1));
        // Centred on the metronome's middle column (its pivot, x + 9).
        const char* metro_txt = metro_on ? "ON" : "OFF";
        TomThumbDrawText(disp_, 4 + 9 - (TomThumbInkWidth(metro_txt) - 1) / 2, 39, metro_txt,
                         true);

        char bpm_txt[8];
        snprintf(bpm_txt, sizeof(bpm_txt), "%d", (int)(tempo_->GetBpm() + 0.5f));
        disp_->SetCursor(30, 12);
        disp_->WriteString(bpm_txt, Font_11x18, true);
        TomThumbDrawText(disp_, 30 + 11 * (int)strlen(bpm_txt) + 3, 29, "BPM", true);

        // Right of the BPM: the "save as default" result, else a locked
        // hint (Bars/BPM can't change while layers hold audio).
        const char* note = tempo_status_[0] != '\0' ? tempo_status_
                           : tempo_->IsLocked()      ? "LOCKED"
                                                     : "";
        if(note[0] != '\0')
            TomThumbDrawText(disp_, disp_->Width() - 1 - TomThumbInkWidth(note), 17, note, true);

        for(int i = 0; i < TempoClock::kBeatsPerBar; i++)
            disp_->DrawRect(30 + i * 24, 33, 30 + i * 24 + 20, 37, true, i == beat);

        const int bars     = tempo_->GetBars();
        const int max_bars = TempoClock::MaxBarsForBpm(tempo_->GetBpm());
        const int pitch    = 96 / max_bars < 12 ? 96 / max_bars : 12;
        for(int i = 0; i < max_bars; i++)
        {
            const int bx = 30 + i * pitch;
            if(i < bars)
                disp_->DrawRect(bx, 41, bx + pitch - 4, 43, true, i == tempo_->GetBarInLoop());
            else
                for(int dx = 0; dx <= pitch - 4; dx += 2)
                    disp_->DrawPixel(bx + dx, 43, true);
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
        DrawFilterCurve(master_filter_mode_, master_filter_cutoff01_, master_filter_res01_);

        // Actual values, not the pickup-tracking array -- see the
        // comment on DrawHome()'s equivalent Vol readout.
        char cutoff_val[8], res_val[8];
        snprintf(cutoff_val, sizeof(cutoff_val), "%d%%",
                  (int)(master_filter_cutoff01_ * 100.f + 0.5f));
        snprintf(res_val, sizeof(res_val), "%d%%", (int)(master_filter_res01_ * 100.f + 0.5f));
        DrawControlRow(kFooterRow1Y, false, kFooterDividerY, "Cutoff", cutoff_val, res_val, "Res");
        DrawControlRow(kFooterRow2Y, true, kFooterInterRowDividerY, "Cycle mode", "", "", "");
    }
    else if(global_page_ == GlobalPage::Reverb)
    // Size/decay for the ONE reverb bus every layer's and engine's Send
    // feeds (see LooperLayer::SetReverbSend01()'s comment for why this
    // isn't per-layer any more). Bypass's own Send lives on Global:Bypass.
    {
        DrawReverbGraph(2, 125, 13, kFooterDividerY - 2, 1.f, RevMark::Size);
        char size_val[8];
        snprintf(size_val, sizeof(size_val), "%d%%", (int)(reverb_size01_ * 100.f + 0.5f));
        DrawControlRow(kFooterRow1Y, false, kFooterDividerY, "Size", size_val, "", "");
        DrawControlRow(kFooterRow2Y, true, kFooterInterRowDividerY, "", "", "", "");
    }
    else if(global_page_ == GlobalPage::Delay)
    // Shared Time/Feedback for the ONE cross-feedback delay bus every
    // engine/layer sends into (see main.cpp's fx_delay_l/r).
    {
        if(delay_sync_)
            snprintf(line1, sizeof(line1), "Sync: On  %d BPM", (int)(tempo_->GetBpm() + 0.5f));
        else
            snprintf(line1, sizeof(line1), "Sync: Off");
        TomThumbDrawText(disp_, 0, 16, line1, true);
        DrawDelayGraph(4, 124, 30, 12, 1.f, true);
        if(SharedDelay::IsOff(delay_time01_))
            TomThumbDrawText(disp_, (disp_->Width() - TomThumbAdvanceWidth("DELAY OFF")) / 2, 40,
                             "DELAY OFF", true);
        char time_val[12], fb_val[8];
        FormatDelayTime(delay_time01_, time_val, sizeof(time_val));
        snprintf(fb_val, sizeof(fb_val), "%d%%", (int)(delay_feedback01_ * 100.f + 0.5f));
        DrawControlRow(kFooterRow1Y, false, kFooterDividerY, "Time", time_val, fb_val,
                         "Feedback");
        DrawControlRow(kFooterRow2Y, true, kFooterInterRowDividerY,
                         delay_sync_ ? "Sync Off" : "Sync Off*", "", "",
                         delay_sync_ ? "Sync On*" : "Sync On");
    }
    else if(global_page_ == GlobalPage::DelayReverb)
    // How much of the delay's output also feeds the reverb -- so echoes
    // sit in the same space as the dry sound (see main.cpp).
    {
        DrawDelayGraph(4, 124, 28, 13, 1.f, false);
        if(SharedDelay::IsOff(delay_time01_))
            TomThumbDrawText(disp_, (disp_->Width() - TomThumbAdvanceWidth("DELAY OFF")) / 2, 40,
                             "DELAY OFF", true);
        char amt_val[8];
        snprintf(amt_val, sizeof(amt_val), "%d%%", (int)(delay_to_reverb01_ * 100.f + 0.5f));
        DrawControlRow(kFooterRow1Y, false, kFooterDividerY, "Dly>Rev", amt_val, "", "");
        DrawControlRow(kFooterRow2Y, true, kFooterInterRowDividerY, "", "", "", "");
    }
    else if(global_page_ == GlobalPage::DelayTone)
    // Ping-Pong (0 = echoes follow the source's own pan, 100% = always
    // bounce L/R) and Tone (each repeat darker) -- see SharedDelay::Bus.
    {
        DrawDelayGraph(8, 124, 28, 13, 1.f, false);
        TomThumbDrawText(disp_, 0, 17, "L", true); // ping-pong: left above, right below
        TomThumbDrawText(disp_, 0, 44, "R", true);
        if(SharedDelay::IsOff(delay_time01_))
            TomThumbDrawText(disp_, (disp_->Width() - TomThumbAdvanceWidth("DELAY OFF")) / 2, 40,
                             "DELAY OFF", true);
        char pp_val[8], tone_val[8];
        snprintf(pp_val, sizeof(pp_val), "%d%%", (int)(delay_pingpong01_ * 100.f + 0.5f));
        snprintf(tone_val, sizeof(tone_val), "%d%%", (int)(delay_tone01_ * 100.f + 0.5f));
        DrawControlRow(kFooterRow1Y, false, kFooterDividerY, "PingPong", pp_val, tone_val,
                         "Tone");
        DrawControlRow(kFooterRow2Y, true, kFooterInterRowDividerY, "", "", "", "");
    }
    else if(global_page_ == GlobalPage::Bypass)
    // Bypass's own sends -- same layout as every engine's own FX page.
    {
        TomThumbDrawText(disp_, 0, 16, bypass_ ? "Bypass: On" : "Bypass: Off", true);
        // The monitored input as heard, and Global:Mixer's Bypass Peak
        // bar -- same scope+meter as every engine's own Mix page.
        DrawMixPageScope(18, bypass_scope_buf_, bypass_scope_capacity_, kMixByp);
        char send_val[8], shared_val[12];
        if(!bypass_fx_target_delay_)
        {
            snprintf(send_val, sizeof(send_val), "%d%%",
                      (int)(bypass_reverb_send01_ * 100.f + 0.5f));
            snprintf(shared_val, sizeof(shared_val), "%d%%",
                      (int)(reverb_size01_ * 100.f + 0.5f));
            DrawControlRow(kFooterRow1Y, false, kFooterDividerY, "Send", send_val, shared_val,
                             "Size");
        }
        else
        {
            snprintf(send_val, sizeof(send_val), "%d%%",
                      (int)(bypass_delay_send01_ * 100.f + 0.5f));
            FormatDelayTime(delay_time01_, shared_val, sizeof(shared_val));
            DrawControlRow(kFooterRow1Y, false, kFooterDividerY, "Send", send_val, shared_val,
                             "Time");
        }
        DrawControlRow(kFooterRow2Y, true, kFooterInterRowDividerY,
                         bypass_fx_target_delay_ ? "Reverb" : "Reverb*", "", "",
                         bypass_fx_target_delay_ ? "Delay*" : "Delay");
    }
    else if(global_page_ == GlobalPage::ReverbTone && !global_reverb_tone_target_b_)
    // PreDelay + PreFilter pair -- gap before the tail starts, and input
    // tone/brightness going into the tank (see DattorroReverb).
    {
        DrawReverbGraph(2, 125, 13, kFooterDividerY - 2, 1.f, RevMark::PreDelay);
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
        DrawReverbGraph(2, 125, 13, kFooterDividerY - 2, 1.f, RevMark::None);
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
        DrawReverbGraph(2, 125, 13, kFooterDividerY - 2, 1.f, RevMark::None);
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

void Ui::DrawReverbGraph(int x0, int x1, int ytop, int ybot, float send01, RevMark mark)
{
    // The dry hit, then (after the pre-delay) a few early reflections and
    // the tail -- see the RevMark comment in ui.h for what moves what.
    const int base = ybot;
    disp_->DrawLine(x0, base, x1, base, true);
    disp_->DrawRect(x0, ytop + 2, x0 + 1, base, true, true);
    const int span = x1 - x0;
    const int gap  = 2 + (int)(reverb_predelay01_ * 0.12f * (float)span);
    const int t0   = x0 + 3 + gap;
    if(mark == RevMark::PreDelay)
    {
        disp_->DrawLine(x0 + 3, ytop, t0 - 1, ytop, true);
        disp_->DrawLine(x0 + 3, ytop, x0 + 3, ytop + 2, true);
        disp_->DrawLine(t0 - 1, ytop, t0 - 1, ytop + 2, true);
    }
    if(send01 < 0.005f)
        return; // nothing sent: just the dry hit
    const int   len = 8 + (int)(reverb_size01_ * 0.85f * (float)(span - gap - 6));
    const float h0  = (float)(ybot - ytop) * (0.45f + 0.45f * reverb_prefilter01_) * send01;
    const float in_diff
        = 0.5f * (reverb_input_diffusion1_01_ + reverb_input_diffusion2_01_);
    const int n_er    = 2 + (int)((1.f - in_diff) * 4.f);
    const int er_step = 2 + (int)((1.f - in_diff) * 2.f);
    for(int k = 0; k < n_er; k++)
    {
        const int x  = t0 + k * er_step;
        const int hh = (int)(h0 * 0.9f * powf(0.85f, (float)k));
        if(x <= x1 && hh > 0)
            disp_->DrawLine(x, base - hh, x, base, true);
    }
    const float tau  = (float)len / 3.2f;
    const int   step = reverb_decay_diffusion01_ > 0.6f ? 1 : reverb_decay_diffusion01_ > 0.3f ? 2 : 3;
    for(int x = t0 + 2; x <= x1 && x <= t0 + len; x++)
    {
        if((x - t0) % step)
            continue; // low decay diffusion: a grainier, gappy tail
        const int hh = (int)(h0 * 0.75f * expf(-(float)(x - t0) / tau));
        if(hh <= 0)
            continue;
        // Damping thins the tail towards its end.
        const float f    = (float)(x - t0) / (float)len;
        const int   skip = 1 + (int)(reverb_damping01_ * f * 3.f);
        for(int y = base - hh; y < base; y++)
            if((x + y) % skip == 0)
                disp_->DrawPixel(x, y, true);
        if(mark == RevMark::Size)
            disp_->DrawPixel(x, base - hh - 2, true); // the envelope Size stretches
    }
}

void Ui::DrawDelayGraph(int x0, int x1, int ymid, int amp, float send01, bool beat_ticks)
{
    disp_->DrawRect(x0, ymid - amp, x0 + 1, ymid + amp, true, true); // the dry hit
    for(int x = x0 + 3; x <= x1; x += 2)
        disp_->DrawPixel(x, ymid, true);
    if(SharedDelay::IsOff(delay_time01_))
        return;
    const float bpm     = tempo_->GetBpm();
    const float ms      = SharedDelay::TimeMs(delay_time01_, delay_sync_, bpm);
    const float beat_ms = 60000.f / (bpm > 1.f ? bpm : 1.f);
    // Enough time across to show a few echoes: four beats when synced.
    float window = delay_sync_ ? fmaxf(4.f * beat_ms, 3.f * ms) : Clampf(6.f * ms, 300.f, 3000.f);
    const int span = x1 - x0;
    if(delay_sync_ && beat_ticks)
        for(float b = beat_ms; b < window; b += beat_ms)
        {
            const int x = x0 + (int)(b / window * (float)span);
            disp_->DrawLine(x, ymid + amp + 2, x, ymid + amp + 3, true);
        }
    int sp = (int)(ms / window * (float)span);
    if(sp < 3)
        sp = 3;
    const float pp   = delay_pingpong01_;
    const bool  dark = delay_tone01_ > 0.5f;
    float       a    = (float)amp * send01;
    for(int n = 1;; n++)
    {
        const int x = x0 + n * sp;
        const int h = (int)(a + 0.5f);
        if(x > x1 - 1 || h < 1 || n > 24)
            break;
        // Ping-pong: odd repeats lean left (up), even ones right (down).
        const int lean = (int)((float)h * pp + 0.5f);
        const int base = h - lean;
        const int top  = base + (n % 2 ? lean : 0);
        const int bot  = base + (n % 2 ? 0 : lean);
        if(dark && n > 1)
            disp_->DrawLine(x, ymid - top, x, ymid + bot, true); // darker: thinner
        else
            disp_->DrawRect(x, ymid - top, x + 1, ymid + bot, true, true);
        // Dly-Rev: a little reverb haze trailing each echo.
        const int haze = (int)((float)sp * 0.8f * delay_to_reverb01_);
        for(int k = 2; k < haze; k++)
        {
            const int hh = (int)((float)(top > bot ? top : bot) * 0.7f
                                 * expf(-(float)k / ((float)sp * 0.3f)));
            for(int y = ymid - hh; y <= ymid + hh; y++)
                if((y + k) % 2 == 0 && y != ymid)
                    disp_->DrawPixel(x + 1 + k, y, true);
        }
        a *= delay_feedback01_;
    }
}

void Ui::DrawFxSends(float rev01, float dly01, int target)
{
    // Reverb on the left, delay on the right, each at this send; the one
    // the knobs set has its label inverted.
    char rv[12], dv[12];
    snprintf(rv, sizeof(rv), "REV %d%%", (int)(rev01 * 100.f + 0.5f));
    if(SharedDelay::IsOff(delay_time01_))
        snprintf(dv, sizeof(dv), "DLY OFF"); // see Global:Delay -- nothing sounds yet
    else
        snprintf(dv, sizeof(dv), "DLY %d%%", (int)(dly01 * 100.f + 0.5f));
    if(target == 0)
        disp_->DrawRect(0, 11, TomThumbAdvanceWidth(rv) + 1, 17, true, true);
    if(target == 1)
        disp_->DrawRect(66, 11, 66 + TomThumbAdvanceWidth(dv) + 1, 17, true, true);
    TomThumbDrawText(disp_, 1, 17, rv, target != 0);
    TomThumbDrawText(disp_, 67, 17, dv, target != 1);
    DrawReverbGraph(2, 60, 19, kFooterDividerY - 2, rev01, RevMark::None);
    disp_->DrawLine(64, 11, 64, kFooterDividerY - 2, true);
    DrawDelayGraph(68, 125, 32, 11, dly01, false);
}

void Ui::DrawFilterCurve(FilterMode mode, float cutoff01, float res01)
{
    // Bass left, treble right; the mode top left, the cutoff marked on
    // the baseline.
    const char* name = mode == FilterMode::LowPass    ? "LOWPASS"
                       : mode == FilterMode::HighPass ? "HIGHPASS"
                       : mode == FilterMode::BandPass ? "BANDPASS"
                                                      : "OFF";
    TomThumbDrawText(disp_, 2, 16, name, true);
    const int x0 = 2, x1 = 125, top = 19, base = 41;
    for(int x = x0; x <= x1; x += 2)
        disp_->DrawPixel(x, base + 2, true);
    const float fc   = (float)x0 + Clampf(cutoff01, 0.f, 1.f) * (float)(x1 - x0);
    const float peak = Clampf(res01, 0.f, 1.f) * 12.f;
    const int   flat = base - (base - top - 12);
    int         prev = -1;
    for(int x = x0; x <= x1; x++)
    {
        const float r = ((float)x - fc) / 12.f;
        float       g, bump = peak * expf(-(r * 1.4f) * (r * 1.4f));
        switch(mode)
        {
            case FilterMode::LowPass: g = 1.f / (1.f + expf(r * 1.6f)); break;
            case FilterMode::HighPass: g = 1.f / (1.f + expf(-r * 1.6f)); break;
            case FilterMode::BandPass:
                g = expf(-(r * 0.55f) * (r * 0.55f));
                bump *= 0.6f;
                break;
            default: g = 1.f; bump = 0.f; break;
        }
        int y = mode == FilterMode::Off ? flat : (int)((float)base - g * (float)(base - top - 12) - bump);
        if(y < top)
            y = top;
        if(prev >= 0)
            disp_->DrawLine(x, prev, x, y, true);
        prev = y;
    }
    if(mode != FilterMode::Off)
        disp_->DrawLine((int)fc, base + 1, (int)fc, base + 3, true);
}

void Ui::DrawGainScope()
{
    // What's recorded: the input times this gain (the monitor signal
    // without its mix volume), against dotted clip lines at full scale.
    const int top = 13, bot = 44, mid = (top + bot) / 2, half = (bot - top) / 2 - 1;
    // "CLIP" at the right end of the top line, centred on it (Tom Thumb
    // is 5px tall above its baseline), the dots stopping short of it.
    const int clip_x = 126 - TomThumbAdvanceWidth("CLIP");
    for(int x = 2; x < 126; x += 3)
    {
        if(x < clip_x - 2)
            disp_->DrawPixel(x, mid - half, true);
        disp_->DrawPixel(x, mid + half, true);
    }
    TomThumbDrawText(disp_, clip_x, mid - half + 3, "CLIP", true);
    const float vol = GetBypassMixVolume();
    if(!bypass_scope_buf_ || bypass_scope_capacity_ == 0 || vol <= 0.0001f)
    {
        disp_->DrawLine(2, mid, 124, mid, true);
        return;
    }
    constexpr int kCols = 61;
    const size_t  stride = bypass_scope_capacity_ / kCols;
    int           prev_y = mid;
    for(int c = 0; c < kCols; c++)
    {
        const float v = Clampf(bypass_scope_buf_[(size_t)c * stride] / vol, -1.f, 1.f);
        const int   y = mid - (int)(v * (float)half);
        const int   x = 2 + c * 2;
        if(c > 0)
            disp_->DrawLine(x - 2, prev_y, x, y, true);
        prev_y = y;
    }
}

void Ui::DrawDriveCurve(float drive01, float trim01)
{
    constexpr float kPi = 3.14159265f;
    // Two cycles of a sine, dotted, and the same driven (solid): Drive
    // squares it off, Trim sets its height.
    const int   top = 12, bot = 44, mid = (top + bot) / 2;
    const float amp  = (float)(bot - top) / 2.f - 1.f;
    const float k    = 1.f + drive01 * 9.f;
    const float trim = trim01 >= 0.5f ? 1.f + (trim01 - 0.5f) * 2.f : trim01 * 2.f;
    int         prev = -1;
    for(int x = 2; x <= 124; x++)
    {
        const float ph = (float)(x - 2) / 122.f * 4.f * kPi;
        const float c  = sinf(ph);
        if(x % 2 == 0)
            disp_->DrawPixel(x, mid - (int)(c * amp), true);
        float d = drive01 > 0.f ? tanhf(k * c) / tanhf(k) : c;
        d       = Clampf(d * (drive01 > 0.f ? trim : 1.f), -1.f, 1.f);
        const int y = mid - (int)(d * amp);
        if(prev >= 0)
            disp_->DrawLine(x, prev, x, y, true);
        prev = y;
    }
}

void Ui::DrawTremoloCurve(float depth01, float rate01)
{
    constexpr float kPi = 3.14159265f;
    // The level over about a second: as many dips as Rate, as deep as
    // Depth -- a steady band when it's off.
    const int   top = 12, bot = 44, mid = (top + bot) / 2, amp = (bot - top) / 2;
    const float cycles = LooperLayer::TremoloHz(rate01) * 0.8f;
    for(int x = 2; x <= 124; x += 2)
    {
        const float lfo = 0.5f * (1.f + cosf((float)(x - 2) / 122.f * 2.f * kPi * cycles));
        const float g   = 1.f - depth01 * (1.f - lfo);
        const int   h   = (int)(g * (float)amp + 0.5f);
        disp_->DrawLine(x, mid - h, x, mid + h, true);
    }
}

const char* Ui::ArpEngineName(int e, bool short_name)
{
    static const char* const kShort[Arpeggiator::kEngines]
        = {"GR", "DXA", "DXB", "ENS", "ORG", "SMV", "SMD", "SSW"};
    static const char* const kLong[Arpeggiator::kEngines]
        = {"Grains", "DXD-A", "DXD-B", "Ensemble", "Organ", "SMPVO", "SMPDR", "Supersaw"};
    if(e < 0 || e >= Arpeggiator::kEngines)
        return "?";
    return short_name ? kShort[e] : kLong[e];
}

Arpeggiator::Settings& Ui::ArpEdit()
{
    return arp_track_ >= 0 ? arp_->PatternSettings(arp_track_, arp_pattern_) : arp_->Get(arp_engine_);
}

int Ui::ArpInst() const
{
    return arp_track_ >= 0 ? Arpeggiator::TrackArp(arp_track_) : arp_engine_;
}

bool Ui::ArpLive() const
{
    // A track's pattern that isn't playing has no live state to show (the
    // track's arp is playing another pattern's settings).
    return arp_track_ < 0 || !seq_ || seq_->GetPattern(arp_track_) == arp_pattern_;
}

void Ui::DrawSeqArpPage(int t)
{
    // The track's patterns, each with its arp's on/off under it; the one
    // K1 chose marked, and what its arp does spelled out.
    char title[16];
    snprintf(title, sizeof(title), "M%d:Arp", t + 1);
    disp_->SetCursor(0, 0);
    WriteUpper(title);
    DrawBeatIndicator(disp_->Width() - 41, 0, 3);
    disp_->DrawLine(0, 9, disp_->Width() - 1, 9, true);
    if(!arp_)
        return;
    if(pod_->button2.Pressed() && !button2_long_fired_)
    {
        // Held B2: copying to every pattern, the same hold bar as a clear.
        const float held = pod_->button2.TimeHeldMs();
        const int   w    = (int)(Clampf(held / 800.f, 0.f, 1.f) * (disp_->Width() - 2));
        disp_->SetCursor(0, 20);
        WriteUpper("Hold: Copy all..");
        disp_->DrawRect(0, 30, disp_->Width() - 1, 34, true, false);
        if(w > 0)
            disp_->DrawRect(1, 31, w, 33, true, true);
        DrawControlRow(kFooterRow1Y, false, kFooterDividerY, "", "", "", "");
        DrawControlRow(kFooterRow2Y, true, kFooterInterRowDividerY, "", "", "", "");
        return;
    }
    const int playing = seq_->GetPattern(t);
    for(int p = 0; p < MidiSequencer::kPatterns; p++)
    {
        const int  x0 = 4 + p * 15, x1 = x0 + 12, y0 = 12, y1 = 24;
        const bool on = arp_->PeekPattern(t, p).on;
        const bool fill = p == playing;
        disp_->DrawRect(x0, y0, x1, y1, true, fill);
        disp_->SetCursor(x0 + 4, y0 + 3);
        disp_->WriteChar((char)('A' + p), Font_6x8, !fill);
        if(on) // the arp's on: a bar under it
            disp_->DrawRect(x0 + 2, y1 + 2, x1 - 2, y1 + 3, true, true);
        if(p == seq_patt_cursor_)
            disp_->DrawRect(x0 - 1, y0 - 1, x1 + 1, y1 + 5, true, false);
    }
    const Arpeggiator::Settings& s = arp_->PeekPattern(t, seq_patt_cursor_);
    char line[40];
    if(!s.on)
        snprintf(line, sizeof(line), "%c: arp off", 'A' + seq_patt_cursor_);
    else if(s.mode == Arpeggiator::kPhrase)
        snprintf(line, sizeof(line), "%c: %s %s", 'A' + seq_patt_cursor_,
                 Arpeggiator::GetPhrase(s.phrase).name, Arpeggiator::RateName(s.rate));
    else
        snprintf(line, sizeof(line), "%c: %s %s %s %s", 'A' + seq_patt_cursor_, Arpeggiator::ShapeName(s.shape),
                 Arpeggiator::RateName(s.rate), Arpeggiator::KeyName(s.key), Arpeggiator::ScaleName(s.scale));
    TomThumbDrawText(disp_, 1, 37, line, true);
    const char* hint = "CLICK ENC TO EDIT";
    TomThumbDrawText(disp_, (disp_->Width() - TomThumbAdvanceWidth(hint)) / 2, 44, hint, true);
    char pat[2] = {(char)('A' + seq_patt_cursor_), 0};
    DrawControlRow(kFooterRow1Y, false, kFooterDividerY, "Pattern", pat, "", "");
    DrawControlRow(kFooterRow2Y, true, kFooterInterRowDividerY, s.on ? "Arp:On" : "Arp:Off", "", "",
                   "Hold=Copy all");
}

void Ui::DrawArpGlobalPage()
{
    // Three columns of engine rows, like Scene edit: its name, an on box
    // and its mode; the chosen engine inverted.
    disp_->SetCursor(0, 0);
    WriteUpper("Global:Arp");
    DrawBeatIndicator(disp_->Width() - 41, 0, 3);
    disp_->DrawLine(0, 9, disp_->Width() - 1, 9, true);
    if(!arp_)
        return;
    for(int c = 1; c <= 2; c++)
        disp_->DrawLine(c * 43 - 1, 11, c * 43 - 1, 36, true);
    for(int c = 0; c < Arpeggiator::kEngines; c++)
    {
        const int                    e  = kEngineShowOrder[c]; // the engine in this cell
        const Arpeggiator::Settings& s  = arp_->Get(e);
        const int  x0 = (c / 3) * 43, y0 = 12 + (c % 3) * 8, base = y0 + 6;
        const bool sel = e == arp_engine_;
        if(sel)
            disp_->DrawRect(x0, y0, x0 + 41, y0 + 7, true, true);
        TomThumbDrawText(disp_, x0 + 2, base, ArpEngineName(e, true), !sel);
        disp_->DrawRect(x0 + 16, y0 + 1, x0 + 20, y0 + 5, !sel, s.on);
        TomThumbDrawText(disp_, x0 + 25, base,
                         !s.on ? "off" : s.mode == Arpeggiator::kPhrase ? "Phr" : "Gen", !sel);
    }
    const char* hint = "CLICK ENC TO EDIT";
    TomThumbDrawText(disp_, (disp_->Width() - TomThumbAdvanceWidth(hint)) / 2, 43, hint, true);
    DrawControlRow(kFooterRow1Y, false, kFooterDividerY, "Engine", ArpEngineName(arp_engine_, false),
                   "", "");
    DrawControlRow(kFooterRow2Y, true, kFooterInterRowDividerY, "On/Off", "", "", "");
    DrawStarToggle(false, "Gen/Phr", arp_->Get(arp_engine_).mode == Arpeggiator::kPhrase);
}

void Ui::DrawStarToggle(bool left, const char* pair, bool second)
{
    // A two-way choice on footer row 2 ("Gen/Phr"): the words stay put and
    // only the star moves -- before the first, after the second.
    const int star = TomThumbAdvanceWidth("*"), words = TomThumbAdvanceWidth(pair);
    const int x    = left ? 1 + star : disp_->Width() - 1 - words - star, base = kFooterRow2Y + 5;
    TomThumbDrawText(disp_, x, base, pair, true);
    TomThumbDrawText(disp_, second ? x + words : x - star, base, "*", true);
}

void Ui::DrawArpPattern(const Arpeggiator::Settings& s)
{
    // The pool as a ladder: a dot per scale note in each step's column
    // (lowest at the bottom, the root's octaves dotted across and
    // labelled), the walk through it drawn as a line, the step playing big.
    // The pool follows the scale and chord, so changing them changes the
    // picture.
    int pool[Arpeggiator::kMaxPool];
    int n = ArpLive() ? arp_->CurrentPool(ArpInst(), pool) : 0;
    if(n == 0)
        n = Arpeggiator::PoolNotes(s, 60 + s.key, pool); // no key yet: from the arp's key
    const int steps = s.steps < 1 ? 1 : s.steps;
    const int top = 12, bot = 44, x0 = 10;
    const int sw = (disp_->Width() - 1 - x0) / steps;
    auto      rung_y = [&](int i) { return bot - (n > 1 ? (i * (bot - top) + (n - 1) / 2) / (n - 1) : 0); };
    for(int i = 0; i < n; i++)
    {
        const int y = rung_y(i);
        if((pool[i] - pool[0]) % 12 == 0)
        {
            for(int x = x0; x < disp_->Width(); x += 2)
                disp_->DrawPixel(x, y, true);
            char lab[4];
            if(i == 0)
                snprintf(lab, sizeof(lab), "R");
            else
                snprintf(lab, sizeof(lab), "+%d", (pool[i] - pool[0]) / 12);
            TomThumbDrawText(disp_, 0, y + 2 > 16 ? y + 2 : 16, lab, true);
        }
        for(int k = 0; k < steps; k++)
            disp_->DrawPixel(x0 + k * sw + sw / 2, y, true);
    }
    const int playing = ArpLive() ? arp_->LastStep(ArpInst()) : -1;
    int       px = -1, py = -1;
    for(int k = 0; k < steps; k++)
    {
        const int x = x0 + k * sw + sw / 2, y = rung_y(Arpeggiator::GenIndex(s, n, k));
        if(px >= 0)
            disp_->DrawLine(px, py, x, y, true);
        if(k == playing)
            disp_->DrawRect(x - 2, y - 2, x + 2, y + 2, true, true);
        else
            disp_->DrawRect(x - 1, y - 1, x + 1, y + 1, true, true);
        px = x;
        py = y;
    }
    char steps_val[4], jump_val[4], shape[16], rep[8];
    snprintf(steps_val, sizeof(steps_val), "%d", steps);
    snprintf(jump_val, sizeof(jump_val), "%d", s.jump);
    snprintf(shape, sizeof(shape), "Shape:%s", Arpeggiator::ShapeName(s.shape));
    snprintf(rep, sizeof(rep), "Rep:x%d", s.repeat);
    DrawControlRow(kFooterRow1Y, false, kFooterDividerY, "Steps", steps_val, jump_val, "Jump");
    DrawControlRow(kFooterRow2Y, true, kFooterInterRowDividerY, shape, "", "", rep);
}

void Ui::DrawArpPhrase(const Arpeggiator::Settings& s)
{
    // The phrase as a piano roll from the key being played (or the arp's
    // key): height its note, the root and its octaves dotted across and
    // labelled; a block's length the note's (ties joined on), its fill the
    // velocity (accent tall, normal solid, soft hollow, ghost a line), a
    // chance step dotted, a ratchet split, a rest a gap; the step playing
    // underlined. Each note clears a pixel round itself off the lines.
    const Arpeggiator::Phrase& ph = Arpeggiator::GetPhrase(s.phrase);
    const bool live = ArpLive();
    const int  root = live && arp_->Root(ArpInst()) >= 0 ? arp_->Root(ArpInst()) : 60 + s.key;
    int        notes[Arpeggiator::kMaxPhraseSteps];
    int        lo = 1000, hi = -1000;
    for(int k = 0; k < ph.len; k++)
    {
        const Arpeggiator::Step& st = ph.steps[k];
        notes[k] = -1;
        if(st.degree == Arpeggiator::kRest || st.degree == Arpeggiator::kTie)
            continue;
        notes[k] = Arpeggiator::PhraseNote(s, root, st.degree) + 12 * (st.octave + s.octave);
        lo       = notes[k] < lo ? notes[k] : lo;
        hi       = notes[k] > hi ? notes[k] : hi;
    }
    const int root_p = root + 12 * s.octave;
    if(lo > hi)
        lo = hi = root_p;
    lo             = root_p < lo ? root_p : lo;
    const int span = hi - lo < 7 ? 7 : hi - lo;
    const int top = 13, bot = 41, x0 = 9;
    const int cw  = (disp_->Width() - x0) / ph.len;
    auto      y   = [&](int p) { return bot - ((p - lo) * (bot - top) * 2 + span) / (2 * span); };
    for(int o = -2; o <= 3; o++)
    {
        const int p = root_p + 12 * o;
        if(p < lo || p > lo + span)
            continue;
        const int yy = y(p);
        for(int x = x0; x < disp_->Width(); x += 3)
            disp_->DrawPixel(x, yy, true);
        char lab[4];
        if(o == 0)
            snprintf(lab, sizeof(lab), "R");
        else
            snprintf(lab, sizeof(lab), "%+d", o);
        const int base = yy + 2 < 16 ? 16 : yy + 2 > 43 ? 43 : yy + 2;
        TomThumbDrawText(disp_, 0, base, lab, true);
    }
    const int playing = live ? arp_->LastStep(ArpInst()) : -1;
    for(int pass = 0; pass < 2; pass++) // clear round every note, then draw them
    {
        for(int k = 0; k < ph.len; k++)
        {
            const Arpeggiator::Step& st = ph.steps[k];
            const int                xa = x0 + k * cw;
            if(notes[k] < 0)
                continue;
            int ties = 0;
            for(int j = k + 1; j < ph.len && ph.steps[j].degree == Arpeggiator::kTie; j++)
                ties++;
            const float frac = st.length == 0 ? 0.45f : st.length == 1 ? 0.75f : 1.f;
            int         w    = (int)((float)cw * ((float)ties + frac)) - (st.length == 2 && ties == 0 ? 0 : 1);
            w                = w < 2 ? 2 : w;
            const int   yy   = y(notes[k]);
            const int   h0   = st.velocity == 3 ? -2 : st.velocity == 0 ? 0 : -1;
            const int   h1   = st.velocity == 0 ? 0 : 1;
            const int   rt   = st.ratchet < 1 ? 1 : st.ratchet;
            for(int i = 0; i < rt; i++)
            {
                int a = rt == 1 ? xa : xa + i * (w + 1) / rt;
                int b = rt == 1 ? xa + w - 1 : xa + (i + 1) * (w + 1) / rt - 2;
                b     = b < a ? a : b;
                if(pass == 0)
                {
                    disp_->DrawRect(a - 1 < x0 ? x0 : a - 1, yy + h0 - 1, b + 1, yy + h1 + 1, false, true);
                    continue;
                }
                if(st.chance)
                    for(int x = a; x <= b; x += 2)
                    {
                        disp_->DrawPixel(x, yy + h0, true);
                        disp_->DrawPixel(x, yy + h1, true);
                    }
                else if(st.velocity == 0)
                    disp_->DrawLine(a, yy, b, yy, true);
                else
                    disp_->DrawRect(a, yy + h0, b, yy + h1, true, st.velocity >= 2);
            }
            if(pass == 1 && k == playing)
                disp_->DrawLine(xa, 44, xa + cw - 2, 44, true);
        }
    }
    char oct[4];
    snprintf(oct, sizeof(oct), s.octave ? "%+d" : "0", s.octave);
    DrawControlRow(kFooterRow1Y, false, kFooterDividerY, "Phrase", ph.name, oct, "Octave");
    DrawControlRow(kFooterRow2Y, true, kFooterInterRowDividerY, "", "", "", "");
}

void Ui::DrawArpScreen()
{
    if(!arp_)
        return;
    const Arpeggiator::Settings& s = ArpEdit();
    static const char* const kPages[Ui::kArpPages] = {"Setup", "Pattern", "Feel", "Key"};
    char title[20];
    const char* page = arp_page_ == 1 && s.mode == Arpeggiator::kPhrase ? "Phrase" : kPages[arp_page_];
    if(arp_track_ >= 0) // a track's pattern's: "M1 B:Setup"
        snprintf(title, sizeof(title), "M%d %c:%s", arp_track_ + 1, 'A' + arp_pattern_, page);
    else
        snprintf(title, sizeof(title), "%s:%s", ArpEngineName(arp_engine_, true), page);
    disp_->SetCursor(0, 0);
    WriteUpper(title);
    DrawBeatIndicator(disp_->Width() - 41, 0, 3);
    disp_->DrawLine(0, 9, disp_->Width() - 1, 9, true);

    if(arp_page_ == 0)
    {
        // Mode and Latch (the buttons) on top; under them the list K1
        // picks from, the one K2 sets inverted.
        const bool phr = s.mode == Arpeggiator::kPhrase;
        const char* latch = s.latch ? "Latch:On" : "Latch:Off";
        TomThumbDrawText(disp_, 2, 17, phr ? "Phrase" : "Generator", true);
        if(!s.on)
            TomThumbDrawText(disp_, 52, 17, "(arp off)", true);
        TomThumbDrawText(disp_, disp_->Width() - 1 - TomThumbAdvanceWidth(latch), 17, latch, true);
        for(int x = 0; x < disp_->Width(); x += 2)
            disp_->DrawPixel(x, 19, true);
        const bool locked = tempo_->IsLocked();
        const int  bpm    = (int)(tempo_->GetBpm() + 0.5f);
        char       tempo_txt[24], range_txt[8], bpm_val[8], range_val[4];
        snprintf(tempo_txt, sizeof(tempo_txt), "%d BPM%s", bpm, locked ? " (Locked)" : "");
        snprintf(range_txt, sizeof(range_txt), "%d oct", s.range);
        snprintf(bpm_val, sizeof(bpm_val), "%d", bpm);
        snprintf(range_val, sizeof(range_val), "%d", s.range);
        static const char* const kRows[3] = {"Rate", "Tempo", "Range"};
        const char* vals[3] = {Arpeggiator::RateName(s.rate), tempo_txt, range_txt};
        for(int i = 0; i < 3; i++)
        {
            const int  y0  = 22 + i * 8;
            const bool sel = i == arp_setup_sel_;
            if(sel)
                disp_->DrawRect(0, y0, disp_->Width() - 1, y0 + 6, true, true);
            TomThumbDrawText(disp_, 2, y0 + 6, kRows[i], !sel);
            TomThumbDrawText(disp_, 34, y0 + 6, vals[i], !sel);
        }
        const char* k2_val = arp_setup_sel_ == 0 ? Arpeggiator::RateName(s.rate)
                             : arp_setup_sel_ == 1 ? (locked ? "Lock" : bpm_val)
                                                   : range_val;
        static const char* const kK2Labels[3] = {"Rate", "BPM", "Oct"};
        DrawControlRow(kFooterRow1Y, false, kFooterDividerY, "Edit", kRows[arp_setup_sel_], k2_val,
                       kK2Labels[arp_setup_sel_]);
        DrawControlRow(kFooterRow2Y, true, kFooterInterRowDividerY, "", "", "", latch);
        DrawStarToggle(true, "Gen/Phr", phr);
        return;
    }
    if(arp_page_ == 1)
    {
        if(s.mode == Arpeggiator::kPhrase)
            DrawArpPhrase(s);
        else
            DrawArpPattern(s);
        return;
    }
    if(arp_page_ == 2)
    {
        // Eight steps: how long each note is held (Gate), every second one
        // pushed late by Swing; height and fill the Variation's loudness.
        const int cw = 14, x0 = 2;
        for(int k = 0; k < 8; k++)
        {
            const Arpeggiator::Flavour f = Arpeggiator::StepFlavour(s.variation, k);
            const int start = x0 + k * cw + ((k & 1) ? (int)(s.swing01 * 0.5f * (float)cw) : 0);
            int       len   = (int)(s.gate01 * f.gate * (float)cw);
            len             = len < 1 ? 1 : len > cw + cw / 2 ? cw + cw / 2 : len;
            const int loud  = f.velocity >= 20 ? 3 : f.velocity >= 0 ? 2 : f.velocity > -20 ? 1 : 0;
            static const int kH[4] = {2, 4, 6, 9};
            if(f.rest)
                disp_->DrawPixel(start + 1, 34, true);
            else if(f.ratchet > 1)
                for(int r = 0; r < f.ratchet; r++)
                    disp_->DrawRect(start + r * cw / f.ratchet, 34 - kH[loud], start + r * cw / f.ratchet + 1,
                                    34, true, true);
            else
                disp_->DrawRect(start, 34 - kH[loud] - (f.octave > 0 ? 6 : 0), start + len - 1,
                                34 - (f.octave > 0 ? 6 : 0), true, loud >= 2);
        }
        for(int x = x0; x < x0 + 8 * cw; x += 2)
            disp_->DrawPixel(x, 36, true);
        for(int k = 0; k <= 8; k++)
            disp_->DrawPixel(x0 + k * cw, 37, true);
        char var[16], swing[16];
        snprintf(var, sizeof(var), s.variation ? "Variation %d" : "Variation off", s.variation);
        // Shown the usual way: 50% straight, 66% triplet, 75% the most.
        const int swing_pct = 50 + (int)(s.swing01 * 25.f + 0.5f);
        snprintf(swing, sizeof(swing), "Swing %d%%", swing_pct);
        TomThumbDrawText(disp_, 2, 44, var, true);
        TomThumbDrawText(disp_, disp_->Width() - 1 - TomThumbAdvanceWidth(swing), 44, swing, true);
        char gate_val[8], swing_val[8];
        snprintf(gate_val, sizeof(gate_val), "%d%%", (int)(s.gate01 * 100.f + 0.5f));
        snprintf(swing_val, sizeof(swing_val), "%d%%", swing_pct);
        DrawControlRow(kFooterRow1Y, false, kFooterDividerY, "Gate", gate_val, swing_val, "Swing");
        DrawControlRow(kFooterRow2Y, true, kFooterInterRowDividerY, "Var -", "", "", "Var +");
        return;
    }
    // Key: an octave of keys from the arp's key, the scale's notes filled,
    // the chord being played dotted. Lock frames the key it's played from.
    const uint16_t mask = Arpeggiator::ScaleMask(s.scale);
    const int      root = ArpLive() ? arp_->Root(ArpInst()) : -1;
    const int      held = s.lock && root >= 0 ? ((root - s.key) % 12 + 12) % 12 : -1;
    const bool     held_chord = ArpLive() && arp_->HoldingChord(ArpInst());
    uint16_t       chord      = 0;
    if(held_chord || (root >= 0 && s.chord != Arpeggiator::kScaleRun))
    {
        int       pool[Arpeggiator::kMaxPool];
        const int n = arp_->CurrentPool(ArpInst(), pool);
        for(int i = 0; i < n; i++)
            chord |= (uint16_t)(1u << (((pool[i] - s.key) % 12 + 12) % 12));
    }
    for(int k = 0; k < 12; k++)
    {
        const int xa = 4 + k * 10;
        disp_->DrawRect(xa, 13, xa + 8, 27, true, (mask >> k) & 1);
        const char* nm = Arpeggiator::KeyName(s.key + k);
        TomThumbDrawText(disp_, xa + (9 - TomThumbAdvanceWidth(nm)) / 2, 34, nm, true);
        if((chord >> k) & 1)
            disp_->DrawRect(xa + 3, 22, xa + 5, 24, (mask >> k) & 1 ? false : true, true);
        if(k == held && !held_chord)
        {
            disp_->DrawRect(xa - 1, 11, xa + 9, 29, true, false);
            disp_->DrawRect(xa + 2, 15, xa + 6, 25, (mask >> k) & 1 ? false : true, false);
        }
    }
    char line[32];
    if(held_chord)
    {
        char name[12];
        arp_->HeldChordName(ArpInst(), name, sizeof(name));
        snprintf(line, sizeof(line), "%s %s  Held: %s", Arpeggiator::KeyName(s.key),
                 Arpeggiator::ScaleName(s.scale), name);
    }
    else if(held >= 0)
        snprintf(line, sizeof(line), "%s %s  Lock, from %s", Arpeggiator::KeyName(s.key),
                 Arpeggiator::ScaleName(s.scale), Arpeggiator::KeyName(root));
    else
        snprintf(line, sizeof(line), "%s %s  %s", Arpeggiator::KeyName(s.key), Arpeggiator::ScaleName(s.scale),
                 s.lock ? "Lock" : "Follow");
    TomThumbDrawText(disp_, 2, 44, line, true);
    char chord_lab[16];
    snprintf(chord_lab, sizeof(chord_lab), "Chord:%s", Arpeggiator::ChordName(s.chord));
    DrawControlRow(kFooterRow1Y, false, kFooterDividerY, "Key", Arpeggiator::KeyName(s.key),
                   Arpeggiator::ScaleName(s.scale), "Scale");
    DrawControlRow(kFooterRow2Y, true, kFooterInterRowDividerY, "", "", "", chord_lab);
    DrawStarToggle(true, "Lock/Follow", !s.lock);
}

int Ui::SendsChannel(int cell)
{
    // Each Global:Sends cell's mixer channel (DXD: the Dexed bus's sends,
    // which both engines share).
    static const int kIds[kSendChannels] = {kMixL1,  kMixL1 + 1, kMixL1 + 2, kMixL1 + 3,
                                            kMixGr,  kMixDxa,    kMixEns,    kMixOrg,
                                            kMixSsw, kMixSmv,    kMixSmd,    kMixByp};
    return kIds[cell < 0 ? 0 : (cell >= kSendChannels ? kSendChannels - 1 : cell)];
}

void Ui::DrawSendsPage()
{
    disp_->SetCursor(0, 0);
    WriteUpper("Global:Sends");
    DrawBeatIndicator(disp_->Width() - 41, 0, 3);
    disp_->DrawLine(0, 9, disp_->Width() - 1, 9, true);

    // A cell per channel, two rows of six: its name, a reverb bar and a
    // delay bar (dotted where empty); the chosen one outlined.
    static const char* const kNames[kSendChannels]
        = {"A1", "A2", "A3", "A4", "GR", "DXD", "ENS", "ORG", "SSW", "SMV", "SMD", "BYP"};
    const bool dly_off = SharedDelay::IsOff(delay_time01_);
    for(int ch = 0; ch < kSendChannels; ch++)
    {
        const int cx = (ch % 6) * 21 + 1, cy = 12 + (ch / 6) * 17;
        if(ch == sends_ch_)
            disp_->DrawRect(cx, cy, cx + 19, cy + 15, true, false);
        TomThumbDrawText(disp_, cx + (20 - TomThumbAdvanceWidth(kNames[ch])) / 2, cy + 6,
                         kNames[ch], true);
        for(int row = 0; row < 2; row++)
        {
            const int   id = SendsChannel(ch);
            const float v  = row == 0 ? MixerGetSend01(id) : (dly_off ? 0.f : MixerGetDelaySend01(id));
            const int   y = cy + 9 + row * 3;
            for(int x = cx + 2; x < cx + 18; x += 2)
                disp_->DrawPixel(x, y + 1, true);
            const int fill = (int)(v * 15.f + 0.5f);
            if(fill > 0)
                disp_->DrawRect(cx + 2, y, cx + 2 + fill, y + 1, true, true);
        }
    }
    char val[8];
    const float v = sends_target_delay_ ? MixerGetDelaySend01(SendsChannel(sends_ch_))
                                          : MixerGetSend01(SendsChannel(sends_ch_));
    if(sends_target_delay_ && dly_off)
        snprintf(val, sizeof(val), "Off");
    else
        snprintf(val, sizeof(val), "%d%%", (int)(v * 100.f + 0.5f));
    DrawControlRow(kFooterRow1Y, false, kFooterDividerY, "Channel", kNames[sends_ch_], val,
                   sends_target_delay_ ? "Dly" : "Rev");
    DrawControlRow(kFooterRow2Y, true, kFooterInterRowDividerY,
                   sends_target_delay_ ? "Reverb" : "Reverb*", "", "",
                   sends_target_delay_ ? "Delay*" : "Delay");
}

void Ui::FormatDelayTime(float time01, char* out, size_t out_size) const
{
    if(SharedDelay::IsOff(time01))
        snprintf(out, out_size, "Off");
    else if(delay_sync_)
        snprintf(out, out_size, "%s", SharedDelay::SyncName(SharedDelay::SyncIndex(time01)));
    else
        snprintf(out, out_size, "%dms",
                  (int)(SharedDelay::TimeMs(time01, false, tempo_->GetBpm()) + 0.5f));
}

// Global:Snap's browse grid reads the highlighted snapshot's settings
// (no audio) once per slot -- static, not on the stack, at ~1 KB.
static PerformanceStore::SynthSnapshot g_snap_browse;

void Ui::RefreshSnapSlots()
{
    snap_slot_count_ = PerformanceStore::ListSynthSnapshots(snap_slots_,
                                                             PerformanceStore::kMaxSynthSnapshots);
    if(PerformanceStore::GetLastError()[0] != '\0')
        snprintf(snap_status_, sizeof(snap_status_), "Read fail:%s",
                  PerformanceStore::GetLastError());
    if(snap_cursor_ >= snap_slot_count_)
        snap_cursor_ = snap_slot_count_ > 0 ? snap_slot_count_ - 1 : 0;
    snap_browse_slot_ = -1; // re-read whatever's highlighted
    snap_slots_dirty_ = false;
}

void Ui::LiveSnapEngineName(int engine, char* out, size_t out_size)
{
    switch(engine)
    {
        case 0: // Grains
            if(!granular_ || granular_->GetSourceLen() == 0)
                snprintf(out, out_size, "Empty");
            else if(granular_loaded_lib_name_[0] != '\0')
                GranularLibDisplayName(granular_loaded_lib_name_, out, out_size);
            else if(granular_loaded_preset_slot_ > 0)
                snprintf(out, out_size, "GRAINS%d", granular_loaded_preset_slot_);
            else
                snprintf(out, out_size, "Capture");
            break;
        case 1: // Dexed -- the patch's own 10-character voice name
        case 4: // DXD-B, the same
        {
            DexedSynth* d    = dexed_engines_[engine == 4 ? 1 : 0];
            char name[11] = {};
            for(int i = 0; i < 10 && d; i++)
                name[i] = (char)d->GetPatchByte(145 + i);
            for(int i = 9; i >= 0 && (name[i] == ' ' || name[i] == '\0'); i--)
                name[i] = '\0';
            snprintf(out, out_size, "%s", name[0] != '\0' ? name : "?");
            break;
        }
        case 7: // Supersaw
            SupersawPresetName(ssw_loaded_slot_, out, out_size);
            break;
        case 2: // Ensemble
        case 5: // Organ
        {
            // The engine not being edited has its slot parked (see
            // SelectEnsembleEngine()).
            const int      e      = engine == 5 ? 1 : 0;
            int            loaded = e == ensemble_active_ ? ensemble_loaded_preset_slot_
                                                          : ensemble_parked_slot_;
            EnsembleSynth* synth  = ensemble_engines_[e];
            if(loaded <= 0 || !synth)
                snprintf(out, out_size, "New");
            else if(EnsembleIsFactorySlot(loaded))
                snprintf(out, out_size, "%s",
                          synth->GetFactoryPresetName(loaded - kEnsembleFactorySlotBase));
            else
                snprintf(out, out_size, "%s%d", e == 1 ? "ORGN" : "ENSM", loaded);
            break;
        }
        default: // 3 = SMPVO, 6 = SMPDR
            if(engine == 6)
            {
                const char* ref   = sample_synth_drum_kit_ref_;
                const char* slash = strrchr(ref, '/');
                snprintf(out, out_size, "%s", ref[0] == '\0' ? "Empty" : (slash ? slash + 1 : ref));
            }
            else if(sample_synth_voice_source_ == SampleSynthVoiceSource::Imported
                    && sample_synth_voice_sound_name_[0] != '\0')
            {
                snprintf(out, out_size, "%s", sample_synth_voice_sound_name_);
                size_t len = strlen(out);
                if(len > 4 && strcasecmp(out + len - 4, ".wav") == 0)
                    out[len - 4] = '\0';
            }
            else if(sample_synth_voice_source_ == SampleSynthVoiceSource::Captured)
                snprintf(out, out_size, "Capture");
            else
                snprintf(out, out_size, "Empty");
            break;
    }
}

void Ui::CaptureSynthSnapshot(PerformanceStore::SynthSnapshot* out)
{
    // Snapshots hold DXD-A, and its loaded slot/unsaved state live in
    // the "active" fields, so make A the active engine first -- and put
    // back whichever the pages were editing when done.
    const int dex_was = dexed_active_, ens_was = ensemble_active_;
    SelectDexedEngine(0);
    SelectEnsembleEngine(0); // the same, for Ensemble and the Organ
    PerformanceStore::SynthSnapshot& snap = *out;
    snap                                  = PerformanceStore::SynthSnapshot{};
    PerformanceStore::SynthSnapshot::Info& in = snap.info;
    in.granular_on        = granular_enabled_;
    in.dexed_on           = dexed_enabled_;
    in.ensemble_on        = ensemble_enabled_;
    in.sampler_voice_on   = sample_synth_voice_enabled_;
    in.sampler_drum_on    = sample_synth_drum_enabled_;
    in.sampler_drum       = sample_synth_mode_ == SampleSynthMode::Drum ? 1 : 0; // face showing
    in.granular_slot      = granular_loaded_preset_slot_;
    in.dexed_slot         = dexed_loaded_preset_slot_;
    in.ensemble_slot      = ensemble_loaded_preset_slot_;
    in.sampler_voice_slot = sample_synth_voice_user_loaded_slot_;
    in.sampler_drum_slot  = sample_synth_drum_user_loaded_slot_;
    snprintf(in.granular_lib_folder, sizeof(in.granular_lib_folder), "%s",
              granular_loaded_lib_folder_);
    snprintf(in.granular_lib_name, sizeof(in.granular_lib_name), "%s", granular_loaded_lib_name_);
    for(int e = 0; e < 4; e++)
        LiveSnapEngineName(e, in.names[e], sizeof(in.names[e]));
    snap.sampler_pair.present = 1;
    LiveSnapEngineName(6, snap.sampler_pair.drum_name, sizeof(snap.sampler_pair.drum_name));

    if(granular_)
    {
        snap.granular              = granular_->CapturePreset();
        snap.granular.midi_channel = granular_midi_channel_;
    }
    if(dexed_engines_[0])
    {
        snap.dexed              = dexed_engines_[0]->CapturePreset();
        snap.dexed.midi_channel = dexed_midi_channel_;
    }
    if(dexed_engines_[0] && dexed_engines_[1])
    {
        // DXD-B is parked while A is active (see SelectDexedEngine()), so
        // its loaded slot is the parked one.
        PerformanceStore::SynthSnapshot::DexedPair& pr = snap.dexed_pair;
        pr.present        = 1;
        pr.b_on           = dexed_b_enabled_ ? 1 : 0;
        pr.b_slot         = dexed_parked_slot_;
        LiveSnapEngineName(4, pr.b_name, sizeof(pr.b_name));
        pr.a_level01      = dexed_engines_[0]->GetEngineLevel01();
        pr.a_pan01        = dexed_engines_[0]->GetEnginePan01();
        pr.b_level01      = dexed_engines_[1]->GetEngineLevel01();
        pr.b_pan01        = dexed_engines_[1]->GetEnginePan01();
        pr.midi_lock      = dexed_midi_lock_ ? 1 : 0;
        pr.b_midi_channel = dexed_b_midi_channel_;
        pr.split_on       = dexed_split_ ? 1 : 0;
        pr.split_note     = dexed_split_note_;
        pr.octave_a       = dexed_octave_[0];
        pr.octave_b       = dexed_octave_[1];
        snap.dexed_b              = dexed_engines_[1]->CapturePreset();
        snap.dexed_b.midi_channel = dexed_b_midi_channel_;
    }
    if(ensemble_engines_[0])
    {
        snap.ensemble              = ensemble_engines_[0]->CapturePreset();
        snap.ensemble.midi_channel = ensemble_midi_channel_;
    }
    if(ensemble_engines_[1])
    {
        // Parked while the Ensemble is active (see above).
        PerformanceStore::SynthSnapshot::Organ& og = snap.organ;
        og.present             = 1;
        og.on                  = organ_enabled_ ? 1 : 0;
        og.slot                = ensemble_parked_slot_;
        LiveSnapEngineName(5, og.name, sizeof(og.name));
        og.preset              = ensemble_engines_[1]->CapturePreset();
        og.preset.midi_channel = organ_midi_channel_;
    }
    if(supersaw_)
    {
        PerformanceStore::SynthSnapshot::Supersaw& ss = snap.supersaw;
        ss.present             = 1;
        ss.on                  = supersaw_enabled_ ? 1 : 0;
        ss.slot                = ssw_loaded_slot_;
        LiveSnapEngineName(7, ss.name, sizeof(ss.name));
        ss.preset              = supersaw_->CapturePreset();
        ss.preset.midi_channel = supersaw_midi_channel_;
    }
    if(sample_synth_)
    {
        // Both faces -- each the same thing its own User preset would
        // save. A Voice capture that was never saved as a Sound has no
        // file to point at, so only its settings go in. The Drum face's
        // Setup state sits in the live fields only while it's showing.
        const SampleSynthMode shown = sample_synth_mode_;
        SelectSampleFace(SampleSynthMode::Drum);
        PerformanceStore::SampleSynthDrumUserPreset& d = snap.sampler_drum;
        snprintf(d.kit_ref, sizeof(d.kit_ref), "%s", sample_synth_drum_kit_ref_);
        d.params             = sample_synth_->CapturePreset();
        d.break_divide_count = (uint8_t)sample_synth_setup_applied_slices_;
        d.break_tune_semis   = sample_synth_setup_tune_semis_;
        d.gain01             = sample_synth_->GetGain01();
        d.vel_sens01         = sample_synth_->GetVelSens01();
        d.midi_channel       = sample_drum_midi_channel_;

        SelectSampleFace(SampleSynthMode::Voice);
        PerformanceStore::SampleSynthVoiceUserPreset& v = snap.sampler_voice;
        if(sample_synth_voice_source_ == SampleSynthVoiceSource::Imported)
        {
            snprintf(v.category, sizeof(v.category), "%s", sample_synth_voice_sound_category_);
            snprintf(v.filename, sizeof(v.filename), "%s", sample_synth_voice_sound_name_);
        }
        v.params       = sample_synth_->CapturePreset();
        v.tune_semis   = sample_synth_imp_tune_semis_;
        v.mono         = sample_synth_imp_mono_;
        v.gain01       = sample_synth_->GetGain01();
        v.vel_sens01   = sample_synth_->GetVelSens01();
        v.midi_channel = sample_synth_midi_channel_;
        SelectSampleFace(shown);
    }
    SelectDexedEngine(dex_was);
    SelectEnsembleEngine(ens_was);
}

void Ui::TriggerSaveSnapshot(bool force_new)
{
    int slot = (!force_new && snap_loaded_slot_ > 0) ? snap_loaded_slot_
                                                     : PerformanceStore::NextFreeSynthSnapshotSlot();
    if(slot < 0)
    {
        snprintf(snap_status_, sizeof(snap_status_), "Card full/missing");
        return;
    }
    static PerformanceStore::SynthSnapshot snap;
    CaptureSynthSnapshot(&snap);

    // Carries the Grains audio, so this is a long write -- same
    // suspend-and-show-progress treatment as TriggerSaveGranularPreset().
    g_audio_suspended = true;
    g_progress_disp   = disp_;
    bool ok = PerformanceStore::SaveSynthSnapshot(
        slot, snap, granular_ ? granular_->GetSourceL() : nullptr,
        granular_ ? granular_->GetSourceR() : nullptr, granular_ ? granular_->GetSourceLen() : 0,
        &Ui::OnSaveLoadProgress);
    g_progress_disp   = nullptr;
    g_audio_suspended = false;

    if(ok)
    {
        snap_loaded_slot_ = slot;
        snap_slots_dirty_ = true; // a new slot now exists
        snprintf(snap_status_, sizeof(snap_status_), "Saved SNAP%d", slot);
    }
    else
    {
        snprintf(snap_status_, sizeof(snap_status_), "Fail:%s", PerformanceStore::GetLastError());
    }
}

void Ui::TriggerLoadSnapshot(int slot)
{
    static PerformanceStore::SynthSnapshot snap;
    size_t audio_len = 0;
    g_audio_suspended = true; // see LoadGranularUserSlot()
    g_progress_disp   = disp_;
    bool ok = PerformanceStore::LoadSynthSnapshot(slot, &snap, granular_capture_buf_l_,
                                                   granular_capture_buf_r_,
                                                   granular_capture_capacity_, &audio_len,
                                                   &Ui::OnSaveLoadProgress);
    g_progress_disp   = nullptr;
    g_audio_suspended = false;
    if(!ok)
    {
        snprintf(snap_status_, sizeof(snap_status_), "Fail:%s", PerformanceStore::GetLastError());
        return;
    }
    bool kit_missing  = ApplySynthSnapshot(snap, audio_len);
    snap_loaded_slot_ = slot;
    snprintf(snap_status_, sizeof(snap_status_),
              kit_missing ? "SNAP%d: kit missing" : "Loaded SNAP%d", slot);
}

bool Ui::ApplySynthSnapshot(PerformanceStore::SynthSnapshot& snap, size_t audio_len)
{
    // Shared by Global:Snap's Load and a Performance load (which carries
    // one, see PerformanceStore::PerformanceExtrasOut). The Grains audio
    // is already in granular_capture_buf_l_/r_. Returns true if the
    // Sampler's kit couldn't be found. DXD-A's slot/unsaved state live in
    // the "active" fields, so A is made active while applying; the pages
    // go back to whichever engine they were editing at the end.
    const int dex_was = dexed_active_;
    SelectDexedEngine(0);
    const PerformanceStore::SynthSnapshot::Info& in = snap.info;

    // Each engine comes back the way its own preset load leaves it --
    // settings, MIDI channel, loaded-slot tracking, a clean "*" -- plus
    // its on/off state. Every voice is released first so nothing hangs
    // over from the sound being replaced.
    if(granular_)
    {
        granular_->AllNotesOff();
        granular_->ApplyPreset(snap.granular);
        ApplyPresetMidiChannel(snap.granular.midi_channel, &granular_midi_channel_);
        granular_->SetSource(granular_capture_buf_l_, granular_capture_buf_r_, audio_len);
        OnNewGranularCapture(audio_len); // clears the library name, so restore it after
        granular_loaded_preset_slot_ = in.granular_slot;
        snprintf(granular_loaded_lib_folder_, sizeof(granular_loaded_lib_folder_), "%s",
                  in.granular_lib_folder);
        snprintf(granular_loaded_lib_name_, sizeof(granular_loaded_lib_name_), "%s",
                  in.granular_lib_name);
        MarkGranularClean();
    }
    granular_enabled_ = in.granular_on;

    if(dexed_engines_[0])
    {
        dexed_engines_[0]->AllNotesOff();
        dexed_engines_[0]->ApplyPreset(snap.dexed);
        // Snapshots saved before MAX VOICES existed were made with all 16
        // voices playing, so they come back that way rather than at the
        // power-on default of 12.
        if(snap.dexed.max_voices < 1)
            dexed_engines_[0]->SetMaxVoices(DexedSynth::kMaxVoices);
        ApplyPresetMidiChannel(snap.dexed.midi_channel, &dexed_midi_channel_);
        dexed_loaded_preset_slot_ = in.dexed_slot;
        dexed_browse_name_slot_   = -2;
        MarkDexedClean();
    }
    dexed_enabled_ = in.dexed_on;

    // DXD-B and the pairing. A snapshot from before they were saved has
    // present == 0 and the defaults: B off, Layer, MIDI locked, both
    // engines at full level and centred -- the old single Dexed.
    if(dexed_engines_[0] && dexed_engines_[1])
    {
        const PerformanceStore::SynthSnapshot::DexedPair& pr = snap.dexed_pair;
        DexedSynth*                                       b  = dexed_engines_[1];
        b->AllNotesOff();
        dexed_b_enabled_ = pr.present && pr.b_on;
        if(pr.present)
        {
            // B's sound only: the shared bus came with A's preset above.
            SelectDexedEngine(1);
            b->ApplyPreset(snap.dexed_b, false);
            dexed_loaded_preset_slot_ = pr.b_slot;
            MarkDexedClean();
            SelectDexedEngine(0);
        }
        dexed_engines_[0]->SetEngineLevel01(pr.a_level01);
        dexed_engines_[0]->SetEnginePan01(pr.a_pan01);
        b->SetEngineLevel01(pr.b_level01);
        b->SetEnginePan01(pr.b_pan01);
        dexed_midi_lock_      = pr.midi_lock != 0;
        dexed_b_midi_channel_ = pr.b_midi_channel;
        dexed_split_          = pr.split_on != 0;
        dexed_split_note_     = pr.split_note < kDexedSplitLowNote    ? kDexedSplitLowNote
                                : pr.split_note > kDexedSplitHighNote ? kDexedSplitHighNote
                                                                      : (int)pr.split_note;
        for(int e = 0; e < 2; e++)
        {
            int o            = e == 0 ? (int)pr.octave_a : (int)pr.octave_b;
            dexed_octave_[e] = o < -2 ? -2 : (o > 2 ? 2 : o);
        }
    }
    ApplyDexedVoiceCeiling(0);

    // Ensemble, then the Organ the same way SelectDexedEngine() is used
    // above. A snapshot from before the Organ has organ.present 0: Organ
    // off, its sound left as it is.
    const int ens_was = ensemble_active_;
    SelectEnsembleEngine(0);
    if(ensemble_engines_[0])
    {
        ensemble_engines_[0]->AllNotesOff();
        ensemble_engines_[0]->ApplyPreset(snap.ensemble);
        ApplyPresetMidiChannel(snap.ensemble.midi_channel, &ensemble_midi_channel_);
        ensemble_loaded_preset_slot_ = in.ensemble_slot;
        MarkEnsembleClean();
    }
    ensemble_enabled_ = in.ensemble_on;
    if(ensemble_engines_[1])
    {
        ensemble_engines_[1]->AllNotesOff();
        organ_enabled_ = snap.organ.present && snap.organ.on;
        if(snap.organ.present)
        {
            SelectEnsembleEngine(1);
            ensemble_->ApplyPreset(snap.organ.preset);
            ApplyPresetMidiChannel(snap.organ.preset.midi_channel, &organ_midi_channel_);
            ensemble_loaded_preset_slot_ = snap.organ.slot;
            MarkEnsembleClean();
            SelectEnsembleEngine(0);
        }
    }
    SelectEnsembleEngine(ens_was);

    // The Supersaw: a snapshot from before it has supersaw.present 0 --
    // off, its sound left as it is.
    if(supersaw_)
    {
        supersaw_->AllNotesOff();
        supersaw_enabled_ = snap.supersaw.present && snap.supersaw.on;
        if(snap.supersaw.present)
        {
            supersaw_->ApplyPreset(snap.supersaw.preset);
            ApplyPresetMidiChannel(snap.supersaw.preset.midi_channel, &supersaw_midi_channel_);
            ssw_loaded_slot_ = snap.supersaw.slot;
            MarkSupersawClean();
        }
    }

    bool kit_missing = false;
    if(sample_synth_)
    {
        // Both faces from a snapshot that has both (sampler_pair), else
        // just the one an older snapshot stored. The face that was
        // showing comes back showing.
        const bool both = snap.sampler_pair.present != 0;
        if(both || in.sampler_drum)
        {
            SelectSampleFace(SampleSynthMode::Drum);
            sample_synth_->AllNotesOff();
            kit_missing = !ApplySampleSynthDrumUserPreset(snap.sampler_drum);
            sample_synth_drum_user_loaded_slot_ = in.sampler_drum_slot;
            MarkSampleSynthClean();
        }
        if(both || !in.sampler_drum)
        {
            SelectSampleFace(SampleSynthMode::Voice);
            sample_synth_->AllNotesOff();
            ApplySampleSynthVoiceUserPreset(snap.sampler_voice);
            sample_synth_voice_user_loaded_slot_ = in.sampler_voice_slot;
            MarkSampleSynthClean();
        }
        SwitchSampleSynthMode(in.sampler_drum ? SampleSynthMode::Drum : SampleSynthMode::Voice);
    }
    sample_synth_voice_enabled_ = in.sampler_voice_on;
    sample_synth_drum_enabled_  = in.sampler_drum_on;
    SelectDexedEngine(dex_was);
    return kit_missing;
}

void Ui::DrawSnapScreen()
{
    disp_->SetCursor(0, 0);
    WriteUpper("Global:Snap");
    DrawBeatIndicator(disp_->Width() - 41, 0, 3);
    disp_->DrawLine(0, 9, disp_->Width() - 1, 9, true);

    if(!PerformanceStore::IsCardPresent())
    {
        const char* msg = "No SD card";
        int         x   = (disp_->Width() - 6 * (int)strlen(msg)) / 2;
        disp_->SetCursor(x < 0 ? 0 : x, 24);
        WriteUpper(msg);
        DrawControlRow(kFooterRow1Y, false, kFooterDividerY, "", "", "", "");
        DrawControlRow(kFooterRow2Y, true, kFooterInterRowDividerY, "Retry", "", "", "");
        return;
    }
    if(snap_slots_dirty_)
        RefreshSnapSlots();

    bool browsing      = save_load_mode_ == SaveLoadMode::BrowsingLoad;
    bool choosing_save = save_load_mode_ == SaveLoadMode::ChoosingSave;

    // Saving keeps its hold-to-confirm bar, same as Global:Perf.
    if(pod_->button2.Pressed() && choosing_save)
    {
        float held = pod_->button2.TimeHeldMs();
        int   w    = (int)(Clampf(held / 800.f, 0.f, 1.f) * (disp_->Width() - 2));
        disp_->SetCursor(0, 20);
        WriteUpper("Hold: Save...");
        disp_->DrawRect(0, 30, disp_->Width() - 1, 34, true, false);
        if(w > 0)
            disp_->DrawRect(1, 31, w, 33, true, true);
        DrawControlRow(kFooterRow1Y, false, kFooterDividerY, "", "", "", "");
        DrawControlRow(kFooterRow2Y, true, kFooterInterRowDividerY, "", "", "", "");
        return;
    }
    if(choosing_save)
    {
        char line1[24], line2[16];
        if(snap_loaded_slot_ > 0)
            snprintf(line1, sizeof(line1), "%c Overwrite %d", !save_as_new_ ? '>' : ' ',
                      snap_loaded_slot_);
        else
            snprintf(line1, sizeof(line1), "  Overwrite (n/a)");
        snprintf(line2, sizeof(line2), "%c Save New", save_as_new_ ? '>' : ' ');
        disp_->SetCursor(0, 24);
        WriteUpper(line1);
        disp_->SetCursor(0, 34);
        WriteUpper(line2);
        DrawControlRow(kFooterRow1Y, false, kFooterDividerY, "Select", "", "", "");
        DrawControlRow(kFooterRow2Y, true, kFooterInterRowDividerY, "Back", "", "", "Hold=Save");
        return;
    }

    // While browsing, the grid shows the highlighted snapshot; otherwise
    // what's live right now.
    const PerformanceStore::SynthSnapshot* shown = nullptr;
    if(browsing && snap_slot_count_ > 0)
    {
        int slot = snap_slots_[snap_cursor_];
        if(slot != snap_browse_slot_)
        {
            snap_browse_ok_   = PerformanceStore::LoadSynthSnapshot(slot, &g_snap_browse);
            snap_browse_slot_ = slot;
        }
        if(snap_browse_ok_)
            shown = &g_snap_browse;
    }

    // Snapshot number, or the last save/load result, on the footer's
    // knob row by K2 -- the body is all engine rows now.
    char top[40] = {};
    if(snap_status_[0] != '\0')
        snprintf(top, sizeof(top), "%s", snap_status_);
    else if(browsing && snap_slot_count_ == 0)
        snprintf(top, sizeof(top), "No snapshots yet");
    else if(browsing)
    {
        int slot = snap_slots_[snap_cursor_];
        snprintf(top, sizeof(top), "%sSNAP%d (%d/%d)%s", slot == snap_loaded_slot_ ? "> " : "",
                  slot, snap_cursor_ + 1, snap_slot_count_, snap_browse_ok_ ? "" : " unreadable");
    }

    const int scroll_key = shown ? snap_browse_slot_ : -1;
    if(scroll_key != snap_name_scroll_key_)
    {
        snap_name_scroll_key_ = scroll_key;
        snap_name_scroll_t0_  = System::GetNow();
    }
    // Two columns of engine rows: short name, on/off dot, then its sound
    // name (scrolling sideways if it's long).
    if(!browsing || shown)
    {
        // Grid order, the same as Global:MIDI's (kEngineShowOrder), in the
        // snapshot's own numbering: 4 is DXD-B, 5 the Organ, 7 the
        // Supersaw, 3/6 SMPVO/SMPDR (see LiveSnapEngineName()).
        static const int kRows[kSnapRows] = {0, 1, 4, 2, 5, 7, 3, 6};
        // An older snapshot holds one Sampler face only (the other
        // column shows "-").
        const bool pair      = shown && shown->sampler_pair.present != 0;
        const bool old_drum  = shown && !pair && shown->info.sampler_drum != 0;
        const bool old_voice = shown && !pair && shown->info.sampler_drum == 0;
        for(int r = 0; r < kSnapRows; r++)
        {
            const int e = kRows[r];
            bool      on;
            char      name[24];
            switch(e)
            {
                case 0:
                    on = shown ? shown->info.granular_on : granular_enabled_;
                    break;
                case 1:
                    on = shown ? shown->info.dexed_on : dexed_enabled_;
                    break;
                case 4:
                    on = shown ? shown->dexed_pair.b_on != 0 : dexed_b_enabled_;
                    break;
                case 2:
                    on = shown ? shown->info.ensemble_on : ensemble_enabled_;
                    break;
                case 5:
                    on = shown ? shown->organ.present && shown->organ.on : organ_enabled_;
                    break;
                case 3:
                    on = shown ? shown->info.sampler_voice_on : sample_synth_voice_enabled_;
                    break;
                case 7:
                    on = shown ? shown->supersaw.present && shown->supersaw.on : supersaw_enabled_;
                    break;
                default:
                    on = shown ? shown->info.sampler_drum_on : sample_synth_drum_enabled_;
                    break;
            }
            if(shown)
            {
                if((e == 3 && old_drum) || (e == 6 && old_voice))
                    snprintf(name, sizeof(name), "-");
                else if(e == 6)
                    snprintf(name, sizeof(name), "%s",
                              pair ? shown->sampler_pair.drum_name : shown->info.names[3]);
                else if(e == 4)
                    snprintf(name, sizeof(name), "%s",
                              shown->dexed_pair.present ? shown->dexed_pair.b_name : "-");
                else if(e == 5)
                    snprintf(name, sizeof(name), "%s",
                              shown->organ.present ? shown->organ.name : "-");
                else if(e == 7)
                    snprintf(name, sizeof(name), "%s",
                              shown->supersaw.present ? shown->supersaw.name : "-");
                else
                    snprintf(name, sizeof(name), "%s", shown->info.names[e]);
            }
            else
                LiveSnapEngineName(e, name, sizeof(name));
            const int x0 = (r / 4) * kSnapColW, y0 = 12 + (r % 4) * 8, base = y0 + 6;
            const int nx = x0 + kSnapNameX;
            // Only the letters wholly inside the name's window, so a
            // scrolling name never runs into the label or the next column.
            int cx = nx - SnapNameScroll(TomThumbAdvanceWidth(name));
            for(const char* c = name; *c; c++)
            {
                const char one[2] = {*c, 0};
                const int  adv    = TomThumbAdvanceWidth(one);
                if(cx >= nx && cx + adv <= nx + kSnapNameW + 1)
                    TomThumbDrawChar(disp_, cx, base, *c, true);
                cx += adv;
            }
            TomThumbDrawText(disp_, x0 + 2, base, kEngineColLabels[kEngineShowOrder[r]], true);
            disp_->DrawRect(x0 + 15, base - 3, x0 + 17, base - 1, true, on); // filled = engine on
        }
        // On the footer's knob circle (x = Width/2).
        disp_->DrawLine(kSnapColW, 11, kSnapColW, kFooterDividerY - 2, true);
    }

    // Next to K2, which scrolls them -- in place of a "Scroll" label.
    DrawControlRow(kFooterRow1Y, false, kFooterDividerY, "", "", "", top);
    DrawControlRow(kFooterRow2Y, true, kFooterInterRowDividerY, browsing ? "Back" : "Save", "",
                     "", browsing ? "Load" : "Open");
}

constexpr const char* Ui::kEngineColLabels[]; // C++14: needs an out-of-class definition
constexpr int         Ui::kEngineShowOrder[]; // likewise

int Ui::SnapNameScroll(int name_len) const
{
    // Hold, scroll the overflow through, hold, start again.
    const int over = name_len - 1 - kSnapNameW;
    if(over <= 0)
        return 0;
    const uint32_t period = 2 * kSnapNameHoldMs + (uint32_t)over * kSnapNameStepMs;
    const uint32_t t      = (System::GetNow() - snap_name_scroll_t0_) % period;
    if(t < kSnapNameHoldMs)
        return 0;
    const int step = (int)((t - kSnapNameHoldMs) / kSnapNameStepMs);
    return step < over ? step : over;
}

void Ui::DrawFileScreen()
{
    disp_->SetCursor(0, 0);
    WriteUpper("Global:Perf");
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

    bool choosing_save = save_load_mode_ == SaveLoadMode::ChoosingSave;
    bool browsing_load = save_load_mode_ == SaveLoadMode::BrowsingLoad;

    // Saving keeps its hold-to-confirm bar; loading is a plain tap.
    if(pod_->button2.Pressed() && choosing_save)
    {
        float held = pod_->button2.TimeHeldMs();
        int   w    = (int)(Clampf(held / 800.f, 0.f, 1.f) * (disp_->Width() - 2));
        disp_->SetCursor(0, 20);
        WriteUpper("Hold: Save...");
        disp_->DrawRect(0, 30, disp_->Width() - 1, 34, true, false);
        if(w > 0)
            disp_->DrawRect(1, 31, w, 33, true, true);
        DrawControlRow(kFooterRow1Y, false, kFooterDividerY, "", "", "", "");
        DrawControlRow(kFooterRow2Y, true, kFooterInterRowDividerY, "", "", "", "");
        return;
    }

    TomThumbDrawText(disp_, 0, 16, "Performances", true);

    if(choosing_save)
    {
        char line1[24], line2[16];
        snprintf(line1, sizeof(line1), "%c Overwrite%s", !save_as_new_ ? '>' : ' ',
                  loaded_slot_ >= 0 ? "" : " (n/a)");
        snprintf(line2, sizeof(line2), "%c Save New", save_as_new_ ? '>' : ' ');
        disp_->SetCursor(0, 24);
        WriteUpper(line1);
        disp_->SetCursor(0, 34);
        WriteUpper(line2);
    }
    else if(browsing_load)
    {
        // "PERF" matches Global:SdMgmt's own short code for this box.
        // "> " marks the performance that's loaded right now.
        char line[32];
        if(file_cursor_ == 0)
            snprintf(line, sizeof(line), "New (blank)");
        else
        {
            int slot = file_slots_[file_cursor_ - 1];
            snprintf(line, sizeof(line), "%sPERF%d", slot == loaded_slot_ ? "> " : "", slot);
        }
        disp_->SetCursor(0, 24);
        WriteUpper(line);
        if(file_status_[0] != '\0')
        {
            disp_->SetCursor(0, 36);
            WriteUpper(file_status_);
        }
    }
    else
    {
        char line1[32];
        if(loaded_slot_ >= 0)
            snprintf(line1, sizeof(line1), "Now: PERF%d", loaded_slot_);
        else
            snprintf(line1, sizeof(line1), "Now: (unsaved)");
        disp_->SetCursor(0, 24);
        WriteUpper(line1);
        if(file_status_[0] != '\0')
        {
            disp_->SetCursor(0, 36);
            WriteUpper(file_status_);
        }
    }

    DrawControlRow(kFooterRow1Y, false, kFooterDividerY, choosing_save ? "Select" : "", "", "",
                     browsing_load ? "Scroll" : "");
    DrawControlRow(kFooterRow2Y, true, kFooterInterRowDividerY,
                     save_load_mode_ == SaveLoadMode::Idle ? "Save" : "Back", "", "",
                     choosing_save ? "Hold=Save" : "Load");
}

const char* Ui::SdMgmtFolderName(SdMgmtFolder f)
{
    switch(f)
    {
        case SdMgmtFolder::Global: return "Global";
        case SdMgmtFolder::GranularPresets: return "Grains Presets";
        case SdMgmtFolder::DexedPresets: return "Dexed Presets";
        case SdMgmtFolder::EnsemblePresets: return "Ens/Org/SSaw Presets";
        case SdMgmtFolder::SampleSynthCaptures: return "Captures";
        case SdMgmtFolder::SampleSynthImports: return "Sampler Imports";
        case SdMgmtFolder::SampleSynthVoiceUserPresets: return "SMPVO User";
        case SdMgmtFolder::SampleSynthDrumUserPresets: return "SMPDR User";
        default: return "?";
    }
}

const char* Ui::SdMgmtFolderShortLabel(SdMgmtFolder f)
{
    switch(f)
    {
        case SdMgmtFolder::Global: return "GLOBAL";
        case SdMgmtFolder::GranularPresets: return "GRAINS";
        case SdMgmtFolder::DexedPresets: return "DEXED";
        case SdMgmtFolder::EnsemblePresets: return "SYNTH"; // Ensemble/Organ/Supersaw
        case SdMgmtFolder::SampleSynthCaptures: return "CAPT";
        case SdMgmtFolder::SampleSynthImports: return "SMPIMP"; // sampler imports -- sounds, kits, breaks
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
    if(pod_->button1.Pressed() && sd_mgmt_in_folder_ && !SdMgmtAtFolderList())
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
    if(pod_->button2.Pressed() && sd_mgmt_in_folder_ && !SdMgmtAtFolderList())
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
        bool bank_box         = SdMgmtIsBankFolder();
        bool at_list          = SdMgmtAtFolderList();
        bool choosing_kind    = sd_mgmt_folder_ == SdMgmtFolder::SampleSynthCaptures
                             && sd_mgmt_capt_kind_ < 0;
        int  bank_level_count = bank_box ? SdMgmtBankLevelCount() : 0;
        int& cur              = at_list ? sd_mgmt_list_cursor_ : sd_mgmt_cursor_;
        if(bank_box && cur >= bank_level_count)
            cur = bank_level_count > 0 ? bank_level_count - 1 : 0;
        if(bank_box && sd_mgmt_bank_ >= 0)
        {
            char label[16];
            SampleBankEntryLabel(sd_mgmt_banks_[sd_mgmt_bank_], label, sizeof(label));
            snprintf(line1, sizeof(line1), "%s%s:", SdMgmtGrainsCaptures() ? "Gr " : "", label);
        }
        else if(sd_mgmt_folder_ == SdMgmtFolder::SampleSynthCaptures && !choosing_kind)
            snprintf(line1, sizeof(line1), "%s Captures:",
                      sd_mgmt_capt_kind_ == 1 ? "Grains" : "Sampler");
        else if(SdMgmtDrumUserBox() && sd_mgmt_drum_kind_ >= 0)
            snprintf(line1, sizeof(line1), "SMPDR %s:", sd_mgmt_drum_kind_ == 1 ? "Breaks" : "Kits");
        else if(SdMgmtGlobalBox() && sd_mgmt_global_kind_ >= 0)
            snprintf(line1, sizeof(line1), "%s:", SdMgmtSnapshots() ? "Snapshots" : "Performances");
        else if(SdMgmtEnsembleBox() && sd_mgmt_ens_kind_ >= 0)
            snprintf(line1, sizeof(line1), "%s:",
                     sd_mgmt_ens_kind_ == 2   ? "Supersaw Presets"
                     : sd_mgmt_ens_kind_ == 1 ? "Organ Presets"
                                              : "Ensemble Presets");
        else if(sd_mgmt_folder_ == SdMgmtFolder::SampleSynthImports && sd_mgmt_smp_kind_ == 0)
            snprintf(line1, sizeof(line1), "%s:",
                      sd_mgmt_smp_sub_kind_ < 0  ? "Voice Imports"
                      : sd_mgmt_smp_sub_kind_ == 1 ? "Multi Samples"
                                                   : "Sound Imports");
        else if(sd_mgmt_folder_ == SdMgmtFolder::SampleSynthImports && sd_mgmt_smp_kind_ == 1)
            snprintf(line1, sizeof(line1), "%s:",
                      sd_mgmt_smp_sub_kind_ < 0  ? "Drum Imports"
                      : sd_mgmt_smp_sub_kind_ == 1 ? "Break Imports"
                                                    : "Kit Imports");
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
            char line2[64];
            if(choosing_kind)
                snprintf(line2, sizeof(line2), "%s", cur == 0 ? "Sampler" : "Grains");
            else if(SdMgmtSmpChoosing())
                snprintf(line2, sizeof(line2), "%s",
                          sd_mgmt_smp_kind_ < 0    ? (cur == 0 ? "Voice" : "Drum")
                          : sd_mgmt_smp_kind_ == 0 ? (cur == 0 ? "Sounds" : "Multi")
                                                   : (cur == 0 ? "Kits" : "Breaks"));
            else if(bank_level_count == 0)
                snprintf(line2, sizeof(line2), "(empty)");
            else if(sd_mgmt_bank_ < 0)
                SampleBankEntryLabel(sd_mgmt_banks_[cur], line2, sizeof(line2));
            else if(SdMgmtGrainsCaptures())
                GranularLibDisplayName(sd_mgmt_item_names_[cur], line2, sizeof(line2));
            else
                snprintf(line2, sizeof(line2), "%s", sd_mgmt_item_names_[cur]);
            disp_->SetCursor(0, 26);
            WriteUpper(line2);
        }
        else
        {
            int        count = 0;
            const int* slots = SdMgmtSlots(&count);
            char       line2[24];
            static const char* const kDrumKinds[2] = {"Kits", "Breaks"};
            if(SdMgmtGlobalBox() && sd_mgmt_global_kind_ < 0)
            {
                // Counts for both, so each choice shows what's inside.
                if(file_slots_dirty_)
                    RefreshFileSlots();
                if(snap_slots_dirty_)
                    RefreshSnapSlots();
                bool snaps = sd_mgmt_list_cursor_ > 0;
                snprintf(line2, sizeof(line2), "%s (%d)", snaps ? "Snapshots" : "Perf",
                          snaps ? snap_slot_count_ : file_slot_count_);
            }
            else if(SdMgmtGlobalBox())
            {
                if(sd_mgmt_cursor_ >= count)
                    sd_mgmt_cursor_ = count > 0 ? count - 1 : 0;
                if(slots && count > 0)
                    snprintf(line2, sizeof(line2), "%s%d", SdMgmtItemPrefix(),
                              slots[sd_mgmt_cursor_]);
                else
                    snprintf(line2, sizeof(line2), "(empty)");
            }
            else if(SdMgmtEnsembleBox() && sd_mgmt_ens_kind_ < 0)
                snprintf(line2, sizeof(line2), "%s",
                         sd_mgmt_list_cursor_ >= 2   ? "Supersaw"
                         : sd_mgmt_list_cursor_ == 1 ? "Organ"
                                                     : "Ensemble");
            else if(SdMgmtEnsembleBox() && slots && count > 0)
                snprintf(line2, sizeof(line2), "%s%d", SdMgmtItemPrefix(), slots[sd_mgmt_cursor_]);
            else if(SdMgmtDrumUserBox() && sd_mgmt_drum_kind_ < 0)
            {
                int k = sd_mgmt_list_cursor_ > 0 ? 1 : 0;
                snprintf(line2, sizeof(line2), "%s (%d)", kDrumKinds[k],
                          sample_synth_drum_user_folder_count_[k]);
            }
            else if(SdMgmtDrumUserBox())
            {
                int n = sample_synth_drum_user_folder_count_[sd_mgmt_drum_kind_];
                if(sd_mgmt_cursor_ >= n)
                    sd_mgmt_cursor_ = n > 0 ? n - 1 : 0;
                if(n > 0)
                    SampleSynthUserSaveName(false, SdMgmtItemIndex(), line2, sizeof(line2));
                else
                    snprintf(line2, sizeof(line2), "(empty)");
            }
            else if(slots && count > 0 && sd_mgmt_folder_ == SdMgmtFolder::DexedPresets)
                // Dexed saves carry their own name -- same as the User page.
                snprintf(line2, sizeof(line2), "%s", DexedSlotName(slots[sd_mgmt_cursor_]));
            else if(slots && count > 0
                    && (sd_mgmt_folder_ == SdMgmtFolder::SampleSynthVoiceUserPresets
                        || sd_mgmt_folder_ == SdMgmtFolder::SampleSynthDrumUserPresets))
                // Same names as the SMPVO/SMPDR User pages.
                SampleSynthUserSaveName(sd_mgmt_folder_ == SdMgmtFolder::SampleSynthVoiceUserPresets,
                                        sd_mgmt_cursor_, line2, sizeof(line2));
            else if(slots && count > 0)
                snprintf(line2, sizeof(line2), "%d", slots[sd_mgmt_cursor_]);
            else
                snprintf(line2, sizeof(line2), "(empty)");
            disp_->SetCursor(0, 26);
            WriteUpper(line2);
        }
    }

    // Shared browse convention: K1 scrolls the grid and folder levels,
    // K2 an item list; B1 backs out a level (hold = Duplicate in an item
    // list), B2 opens (hold = Delete in an item list).
    bool at_list  = SdMgmtAtFolderList();
    bool in_items = sd_mgmt_in_folder_ && !at_list;
    DrawControlRow(kFooterRow1Y, false, kFooterDividerY, in_items ? "" : "Scroll", "", "",
                     in_items ? "Scroll" : "");
    DrawControlRow(kFooterRow2Y, true, kFooterInterRowDividerY,
                     !sd_mgmt_in_folder_ ? "" : (at_list ? "Back" : "Back/Duplicate"), "", "",
                     in_items ? "Hold=Delete" : "Open");
}

int Ui::LiveExportMaxLoops() const
{
    const size_t loop = tempo_->GetLoopLengthSamples();
    if(!live_ || loop == 0)
        return 8;
    const int n = (int)(live_->Capacity() / loop);
    return n < 1 ? 1 : (n > 8 ? 8 : n);
}

void Ui::StartLiveExport(bool for_microdexed)
{
    if(!live_ || live_open_)
        return;
    if(!looper_enabled_)
    {
        // The looper's clock is what finds bar 1 and counts the loops.
        snprintf(export_status_, sizeof(export_status_), "Turn the looper on");
        return;
    }
    // It records into the undo buffer: overdubs carry on while it's lent
    // out, but there's nothing to undo afterwards.
    LooperLayer::BlockUndoBackup(true);
    if(undo_owner_layer_)
        *undo_owner_layer_ = -1;
    if(undo_backup_len_)
        *undo_backup_len_ = 0;
    const int loops     = export_loops_ < LiveExportMaxLoops() ? export_loops_ : LiveExportMaxLoops();
    live_->frames       = 0;
    live_->loops_done   = 0;
    live_->full         = false;
    live_->loops_target = (uint32_t)loops;
    live_44k_           = for_microdexed;
    live_open_          = true;
    export_status_[0]   = '\0';
    live_->state        = LiveExport::kArmed; // last: the callback starts watching for bar 1
}

void Ui::StopLiveExport()
{
    if(!live_open_)
        return;
    if(live_->song)
    {
        StopSong();
        if(live_->state == LiveExport::kArmed)
        {
            // Nothing recorded yet: drop the empty file.
            live_->state      = LiveExport::kIdle;
            live_->song       = false;
            live_->mute       = false;
            g_audio_suspended = true;
            PerformanceStore::LiveExportCancel();
            g_audio_suspended = false;
            LooperLayer::BlockUndoBackup(false);
            live_open_ = false;
            snprintf(export_status_, sizeof(export_status_), "Cancelled");
        }
        else if(live_->state == LiveExport::kRecording)
            live_->state = LiveExport::kDone; // UpdateSongExport() writes the rest
        return;
    }
    if(live_->state == LiveExport::kArmed)
    {
        live_->state = LiveExport::kIdle; // nothing recorded yet: no file
        LooperLayer::BlockUndoBackup(false);
        live_open_ = false;
        snprintf(export_status_, sizeof(export_status_), "Cancelled");
        return;
    }
    if(live_->state == LiveExport::kRecording)
        live_->state = LiveExport::kDone; // UpdateLiveExport() saves what's there
}

bool Ui::WriteLiveFrames(uint32_t frames)
{
    bool ok = true;
    for(uint32_t done = 0; ok && done < frames;)
    {
        const uint32_t seg   = done / live_->seg_frames;
        const uint32_t start = done % live_->seg_frames;
        uint32_t       n     = frames - done;
        if(n > 16384)
            n = 16384;
        if(start + n > live_->seg_frames)
            n = live_->seg_frames - start; // to the end of this segment
        ok = PerformanceStore::LiveExportWrite(live_->seg[seg] + start * 2, n);
        done += n;
        OnSaveLoadProgress((float)done / (float)frames);
    }
    return ok;
}

void Ui::StartSongExport(bool for_microdexed)
{
    if(!live_ || live_open_)
        return;
    if(!looper_enabled_)
    {
        snprintf(export_status_, sizeof(export_status_), "Turn the looper on");
        return;
    }
    if(song_.len == 0)
    {
        snprintf(export_status_, sizeof(export_status_), "The song is empty");
        return;
    }
    // The file first (with the audio paused, as every write), then the
    // song starts and the capture with it, on the next bar 1.
    g_audio_suspended = true;
    bool ok = PerformanceStore::LiveExportOpen(for_microdexed, live_name_, sizeof(live_name_));
    g_audio_suspended = false;
    if(!ok)
    {
        snprintf(export_status_, sizeof(export_status_), "Fail:%s", PerformanceStore::GetLastError());
        return;
    }
    LooperLayer::BlockUndoBackup(true);
    if(undo_owner_layer_)
        *undo_owner_layer_ = -1;
    if(undo_backup_len_)
        *undo_backup_len_ = 0;
    live_->frames        = 0;
    live_->loops_done    = 0;
    live_->full          = false;
    live_->flush_req     = false;
    live_->song          = true;
    live_->mute          = true;
    live_44k_            = for_microdexed;
    live_open_           = true;
    song_export_written_ = 0;
    song_tail_           = false;
    export_status_[0]    = '\0';
    StartSong(song_cursor_);
    live_->state = LiveExport::kArmed; // last: the callback starts watching for bar 1
}

void Ui::UpdateSongExport()
{
    LiveExport& lx = *live_;
    if(lx.state == LiveExport::kArmed)
        return;
    const uint32_t sr = (uint32_t)tempo_->GetSampleRate();
    if(lx.state == LiveExport::kRecording)
    {
        // The song's over (it switched everything off): carry on until
        // it's gone quiet for a moment -- reverb and delay tails -- or 10s.
        const uint32_t total = song_export_written_ + lx.frames;
        if(!song_playing_ && !song_tail_)
        {
            song_tail_       = true;
            song_tail_start_ = total;
            song_quiet_from_ = 0;
        }
        if(song_tail_)
        {
            if(g_master_level < 0.002f)
            {
                if(song_quiet_from_ == 0)
                    song_quiet_from_ = total > 0 ? total : 1;
            }
            else
                song_quiet_from_ = 0;
            if((song_quiet_from_ != 0 && total - song_quiet_from_ >= sr / 4)
               || total - song_tail_start_ >= 10 * sr)
                lx.state = LiveExport::kDone;
        }
    }
    const bool done = lx.state == LiveExport::kDone;
    if(!lx.flush_req && !done)
        return;
    // Everything stops while the chunk goes to the card, then picks up
    // exactly where it was -- no gap in the file.
    g_audio_suspended = true;
    g_progress_disp   = disp_;
    OnSaveLoadProgress(0.f);
    const uint32_t frames = lx.frames;
    bool           ok     = WriteLiveFrames(frames);
    song_export_written_ += frames;
    lx.frames    = 0;
    lx.flush_req = false;
    g_progress_disp = nullptr;
    if(ok && !done)
    {
        g_audio_suspended = false;
        return;
    }
    // Finished (or the card failed): close it, with what's there.
    const bool closed = PerformanceStore::LiveExportClose();
    g_audio_suspended = false;
    StopSong();
    lx.state = LiveExport::kIdle;
    lx.song  = false;
    lx.mute  = false;
    LooperLayer::BlockUndoBackup(false);
    live_open_ = false;
    const uint32_t sec = song_export_written_ / sr;
    if(!ok || !closed)
        snprintf(export_status_, sizeof(export_status_), "Fail:%s", PerformanceStore::GetLastError());
    else
        snprintf(export_status_, sizeof(export_status_), "Saved %s %d:%02d", live_name_,
                 (int)(sec / 60), (int)(sec % 60));
}

void Ui::UpdateLiveExport()
{
    if(live_ && live_open_ && live_->song)
    {
        UpdateSongExport();
        return;
    }
    if(!live_ || !live_open_ || live_->state != LiveExport::kDone)
        return;
    // Recorded: save it with the audio paused, like any other save (the
    // card isn't reliable written to while it runs).
    const uint32_t frames = live_->frames;
    bool           ok     = frames > 0;
    if(ok)
    {
        g_audio_suspended = true;
        g_progress_disp   = disp_;
        OnSaveLoadProgress(0.f);
        ok = PerformanceStore::LiveExportOpen(live_44k_, live_name_, sizeof(live_name_));
        for(uint32_t done = 0; ok && done < frames;)
        {
            const uint32_t seg   = done / live_->seg_frames;
            const uint32_t start = done % live_->seg_frames;
            uint32_t       n     = frames - done;
            if(n > 16384)
                n = 16384;
            if(start + n > live_->seg_frames)
                n = live_->seg_frames - start; // to the end of this segment
            ok = PerformanceStore::LiveExportWrite(live_->seg[seg] + start * 2, n);
            done += n;
            OnSaveLoadProgress((float)done / (float)frames);
        }
        if(ok)
            ok = PerformanceStore::LiveExportClose();
        else
            PerformanceStore::LiveExportClose();
        g_progress_disp   = nullptr;
        g_audio_suspended = false;
    }
    live_->state = LiveExport::kIdle;
    LooperLayer::BlockUndoBackup(false);
    live_open_ = false;
    if(frames == 0)
        snprintf(export_status_, sizeof(export_status_), "Nothing recorded");
    else if(!ok)
        snprintf(export_status_, sizeof(export_status_), "Fail:%s", PerformanceStore::GetLastError());
    else
        snprintf(export_status_, sizeof(export_status_), live_->full ? "Saved %s (full)" : "Saved %s",
                  live_name_);
}

void Ui::DrawExportScreen()
{
    disp_->SetCursor(0, 0);
    WriteUpper("Global:Export");
    DrawBeatIndicator(disp_->Width() - 41, 0, 3);
    disp_->DrawLine(0, 9, disp_->Width() - 1, 9, true);

    // Same layout as the engine pages (see DrawGlobalEntryPage()): a
    // status line, the graphic, and the last result underneath.
    const bool card = PerformanceStore::IsCardPresent();
    TomThumbDrawText(disp_, 1, 16,
                     !card          ? "NO SD CARD"
                     : export_mode_ == kExportSong ? "SONG: PLAYS IT, SILENTLY"
                     : export_mode_ == kExportLive ? "EVERYTHING YOU HEAR, LIVE"
                                                   : "THE AUDIO LAYERS, NO MIDI",
                     true);
    DrawBitmap(disp_, (disp_->Width() - kGlobalIconW) / 2, 18, kGlobalIconW, kGlobalIconH,
               kGlobalIconExport);
    if(live_open_)
    {
        // A live export running: waiting for bar 1, then which loop.
        char msg[32];
        if(live_->song)
        {
            const uint32_t sec = (song_export_written_ + live_->frames)
                                 / (uint32_t)tempo_->GetSampleRate();
            if(live_->state == LiveExport::kArmed)
                snprintf(msg, sizeof(msg), "WAITING FOR BAR 1");
            else if(song_tail_)
                snprintf(msg, sizeof(msg), "ENDING %d:%02d", (int)(sec / 60), (int)(sec % 60));
            else
                snprintf(msg, sizeof(msg), "STEP %d/%d  %d:%02d", song_step_ + 1, song_.len,
                         (int)(sec / 60), (int)(sec % 60));
        }
        else if(live_->state == LiveExport::kArmed)
            snprintf(msg, sizeof(msg), "WAITING FOR BAR 1");
        else if(live_->state == LiveExport::kRecording)
            snprintf(msg, sizeof(msg), "RECORDING LOOP %d/%d", (int)live_->loops_done + 1,
                      (int)live_->loops_target);
        else
            snprintf(msg, sizeof(msg), "SAVING...");
        if(live_->state == LiveExport::kRecording && live_->loops_target > 0 && !live_->song)
        {
            // How far through the whole capture.
            float p = (float)live_->frames
                      / (float)(tempo_->GetLoopLengthSamples() * live_->loops_target);
            disp_->DrawRect(0, 36, disp_->Width() - 1, 38, true, false);
            int w = (int)(Clampf(p, 0.f, 1.f) * (float)(disp_->Width() - 2));
            if(w > 0)
                disp_->DrawRect(1, 37, w, 37, true, true);
        }
        TomThumbDrawText(disp_, (disp_->Width() - TomThumbAdvanceWidth(msg)) / 2, 44, msg, true);
    }
    else if(card && export_status_[0] != '\0')
    {
        char msg[sizeof(export_status_)];
        size_t n = 0;
        for(; export_status_[n] != '\0' && n + 1 < sizeof(msg); n++)
            msg[n] = (char)toupper((unsigned char)export_status_[n]);
        msg[n] = '\0';
        TomThumbDrawText(disp_, (disp_->Width() - TomThumbAdvanceWidth(msg)) / 2, 44, msg, true);
    }

    if(!card)
    {
        DrawControlRow(kFooterRow1Y, false, kFooterDividerY, "", "", "", "");
        DrawControlRow(kFooterRow2Y, true, kFooterInterRowDividerY, "Retry", "", "", "");
        return;
    }

    // No hold-to-confirm here (unlike File's New/Load) -- Export always
    // creates a new numbered file and never overwrites/destroys anything,
    // so a plain tap is safe.
    // K1 Audio Loop (the layers, offline -- quick) / Audio+MIDI Loop (the
    // master output as it plays, MIDI tracks and engines included) / Song;
    // K2 the loop count (capped at what fits in memory, ~38s) or, for a
    // song, nothing -- it starts from the Song page's step.
    const int max_loops = LiveExportMaxLoops();
    const int n_loops   = export_loops_ < max_loops ? export_loops_ : max_loops;
    char      loops[12];
    snprintf(loops, sizeof(loops), "%d loop%s", n_loops, n_loops > 1 ? "s" : "");
    char from[8];
    snprintf(from, sizeof(from), "%d", song_cursor_ < song_.len ? song_cursor_ + 1 : 1);
    // K1's choice takes the whole left side -- the names are too long
    // to sit beside a label.
    if(export_mode_ == kExportSong)
        DrawControlRow(kFooterRow1Y, false, kFooterDividerY, "Song", "", from, "From step");
    else if(export_mode_ == kExportLive)
        DrawControlRow(kFooterRow1Y, false, kFooterDividerY, "Audio+MIDI Loop", "", loops, "Loops");
    else
        DrawControlRow(kFooterRow1Y, false, kFooterDividerY, "Audio Loop", "", "", "");
    if(live_open_)
        DrawControlRow(kFooterRow2Y, true, kFooterInterRowDividerY, "Stop", "", "", "Stop");
    else
        DrawControlRow(kFooterRow2Y, true, kFooterInterRowDividerY, "STUDIO 48K", "", "",
                         "CD 44.1K");
}

void Ui::SetPedalReversed(bool reversed)
{
    if(reversed == pedal_reversed_)
        return;
    // Saved straight away, same as Global:Looper's Default.
    if(!PerformanceStore::IsCardPresent())
        PerformanceStore::Remount();
    g_audio_suspended = true; // see TriggerSave() -- a blocking SD write
    bool ok           = PerformanceStore::SavePedalReversed(reversed);
    g_audio_suspended = false;
    // Applied either way -- it just won't survive a power cycle.
    pedal_reversed_ = reversed;
    if(ok)
        midi_status_[0] = '\0';
    else
        snprintf(midi_status_, sizeof(midi_status_), "%s",
                  PerformanceStore::IsCardPresent() ? "Not saved" : "No SD card");
}

float Ui::MidiChannelKnobRaw(int ch)
{
    return Clampf(((float)(ch + 1) + 0.5f) / 16.99f, 0.f, 1.f);
}

int* Ui::MidiDefaultChannel(int engine)
{
    switch(engine)
    {
        case 0: return &granular_midi_channel_;
        case 1: return &dexed_midi_channel_;
        case 2: return &dexed_b_midi_channel_;
        case 3: return &ensemble_midi_channel_;
        case 4: return &organ_midi_channel_;
        case 5: return &sample_synth_midi_channel_;
        case 7: return &supersaw_midi_channel_;
        default: return &sample_drum_midi_channel_;
    }
}

void Ui::TriggerSaveMidiDefaults()
{
    int channels[PerformanceStore::kMidiDefaultEngines];
    for(int e = 0; e < PerformanceStore::kMidiDefaultEngines; e++)
        channels[e] = *MidiDefaultChannel(e);
    if(PerformanceStore::SaveMidiDefaults(channels))
        snprintf(midi_status_, sizeof(midi_status_), "Saved as default");
    else
        snprintf(midi_status_, sizeof(midi_status_), "Fail:%s", PerformanceStore::GetLastError());
}

int* Ui::MidiRowChannel(int row)
{
    switch(row)
    {
        case 0: return &granular_midi_channel_;
        case 1: return &dexed_midi_channel_;
        // Locked, B plays on A's channel, so that's the one shown and set.
        case 2: return dexed_midi_lock_ ? &dexed_midi_channel_ : &dexed_b_midi_channel_;
        case 3: return &ensemble_midi_channel_;
        case 4: return &organ_midi_channel_;
        case 5: return &sample_synth_midi_channel_;
        case 7: return &supersaw_midi_channel_;
        default: return &sample_drum_midi_channel_;
    }
}

void Ui::DrawMidiScreen()
{
    disp_->SetCursor(0, 0);
    WriteUpper("Global:MIDI");
    DrawBeatIndicator(disp_->Width() - 41, 0, 3);
    disp_->DrawLine(0, 9, disp_->Width() - 1, 9, true);

    // B2 held: the same hold-to-confirm bar as Global:Tempo's Hold=Default.
    if(pod_->button2.Pressed() && !button2_long_fired_)
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

    // Live sustain pedal, polarity applied: filled while held down,
    // outlined when up, hidden until the pedal first sends anything.
    if(pedal_state_ >= 0)
    {
        const bool down = pedal_state_ != 0;
        disp_->DrawRect(68, 0, 82, 7, true, down);
        TomThumbDrawText(disp_, 70, 6, "SUS", !down);
    }

    // One column per engine, same columns as Global:Snap: short name,
    // on/off dot, MIDI channel. K1's column is inverted.
    for(int c = 0; c < kMidiRows; c++)
    {
        const int r = kEngineShowOrder[c]; // the engine in this column
        bool      on;
        switch(r)
        {
            case 0: on = granular_enabled_; break;
            case 1: on = dexed_enabled_; break;
            case 2: on = dexed_b_enabled_; break;
            case 3: on = ensemble_enabled_; break;
            case 4: on = organ_enabled_; break;
            case 5: on = sample_synth_voice_enabled_; break;
            case 7: on = supersaw_enabled_; break;
            default: on = sample_synth_drum_enabled_; break;
        }
        const int   ch    = *MidiRowChannel(r);
        const bool  sel   = r == midi_row_;
        const int   x     = kEngineCol0X + c * kEngineColW;
        const char* label = kEngineColLabels[r];
        if(sel)
            disp_->DrawRect(x, 10, x + kEngineColW - 2, 44, true, true);
        TomThumbDrawText(disp_, x + (kEngineColW - TomThumbInkWidth(label)) / 2, 17, label, !sel);
        disp_->DrawRect(x + 6, 21, x + 8, 23, !sel, on); // filled = engine on
        char ch_txt[8];
        if(ch < 0)
            snprintf(ch_txt, sizeof(ch_txt), "Omni");
        else
            snprintf(ch_txt, sizeof(ch_txt), "%d", ch + 1);
        // Centred under the on/off dot (x + 7). Tom Thumb's "1" has its
        // stem in its right column, so "1" and "10".."16" sit a pixel
        // further left to line that stem up.
        int ch_x = x + (kEngineColW - TomThumbInkWidth(ch_txt)) / 2 - (ch_txt[0] == '1' ? 1 : 0);
        TomThumbDrawText(disp_, ch_x, 32, ch_txt, !sel);
        // Locked, DXD-B plays on A's channel (the one shown).
        if(r == 2 && dexed_midi_lock_)
            TomThumbDrawText(disp_, x + (kEngineColW - TomThumbInkWidth("=A")) / 2, 40, "=A",
                             !sel);
    }

    DrawControlRow(kFooterRow1Y, false, kFooterDividerY, "Engine",
                     midi_status_, "", "Channel");
    DrawControlRow(kFooterRow2Y, true, kFooterInterRowDividerY,
                     pedal_reversed_ ? "Sus-Pedal:Rev" : "Sus-Pedal:Norm", "", "",
                     "Hold=Default");
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
        case GranularParamPage::Position: page_name = "Position"; break;
        case GranularParamPage::Rhythm: page_name = "Rhythm"; break;
        case GranularParamPage::Scan: page_name = "Scan"; break;
        case GranularParamPage::Loop: page_name = "Loop"; break;
        case GranularParamPage::Pitch: page_name = "Pitch"; break;
        case GranularParamPage::ADSR: page_name = "ADSR"; break;
        case GranularParamPage::Filter: page_name = "Filter"; break;
        case GranularParamPage::Mix: page_name = "Mix"; break;
        case GranularParamPage::FX: page_name = "GR-FX"; break;
        case GranularParamPage::MIDI: page_name = "MIDI"; break;
        case GranularParamPage::Capture: page_name = "Capture"; break;
        case GranularParamPage::Trim: page_name = "Trim"; break;
        case GranularParamPage::Preset: page_name = "Preset"; break;
        case GranularParamPage::User: page_name = "User"; break;
        default: break;
    }
    // Shared content band for the simpler pages below (ADSR's graph,
    // Filter's oscilloscope) -- same values as Pad's own kBandTop/
    // kBandBottom. Grain's own page uses a deeper band (see
    // DrawGranularWaveform()) since it also has the two-row Size/Fill/
    // Gap/Scan header above it that these pages don't.
    const int kBandTop = 22, kBandBottom = 44;
    char title[24];
    snprintf(title, sizeof(title), "Gran:%s", page_name);
    DrawPageTitle(title, GranularUnsaved());
    DrawBeatIndicator(disp_->Width() - 41, 0, 3);
    disp_->DrawLine(0, 9, disp_->Width() - 1, 9, true);

    switch(granular_param_page_)
    {
        case GranularParamPage::Grain:
        {
            // Live values across the top, each centered on its own label
            // (Tom Thumb, four equal columns), then the waveform with
            // Position's span and every voice's pointer.
            const char* grain_labels[4] = {"SIZE", "DENSITY", "SPRAY", "SCAN"};
            char        grain_values[4][10];
            FormatGranularSize(granular_, grain_values[0], sizeof(grain_values[0]));
            FormatGranularDensity(granular_, grain_values[1], sizeof(grain_values[1]));
            snprintf(grain_values[2], sizeof(grain_values[2]), "%dms",
                      (int)(granular_->GetSprayMs() + 0.5f));
            FormatGranularScan(granular_, grain_values[3], sizeof(grain_values[3]));
            const int kGrainLabelBaseline = 16; // clear of the title's line
            const int kGrainValueBaseline = 22;
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
            DrawGranularWaveformLive();

            char sz_val[10], dn_val[10];
            FormatGranularSize(granular_, sz_val, sizeof(sz_val));
            FormatGranularDensity(granular_, dn_val, sizeof(dn_val));
            DrawControlRow(kFooterRow1Y, false, kFooterDividerY, "Size", sz_val, dn_val,
                             "Density");
            DrawControlRow(kFooterRow2Y, true, kFooterInterRowDividerY, "", "", "", "");
            break;
        }
        case GranularParamPage::Position:
        {
            char line1[24];
            snprintf(line1, sizeof(line1), "Spray +/-%dms", (int)(granular_->GetSprayMs() + 0.5f));
            TomThumbDrawText(disp_, 0, 17, line1, true); // clear of the title's line
            DrawGranularWaveformLive();

            char pos_val[8], spray_val[10];
            snprintf(pos_val, sizeof(pos_val), "%d%%",
                      (int)(granular_->GetPosition01() * 100.f + 0.5f));
            snprintf(spray_val, sizeof(spray_val), "%dms", (int)(granular_->GetSprayMs() + 0.5f));
            DrawControlRow(kFooterRow1Y, false, kFooterDividerY, "Position", pos_val, spray_val,
                             "Spray");
            DrawControlRow(kFooterRow2Y, true, kFooterInterRowDividerY, "", "", "", "");
            break;
        }
        case GranularParamPage::Rhythm:
        {
            // The pattern itself: 16 steps, filled = a grain fires.
            uint16_t  mask   = granular_->GetRhythmMask();
            const int kStepW = 7, kStepH = 9, kTop = 25;
            const int x0     = (disp_->Width() - GranularEngine::kRhythmSteps * kStepW) / 2;
            for(int st = 0; st < GranularEngine::kRhythmSteps; st++)
            {
                int x = x0 + st * kStepW;
                if((mask >> st) & 1u)
                    disp_->DrawRect(x, kTop, x + kStepW - 2, kTop + kStepH, true, true);
                else
                    disp_->DrawRect(x, kTop, x + kStepW - 2, kTop + kStepH, true, false);
            }
            char line1[32];
            snprintf(line1, sizeof(line1), "Rhythm: %s   Speed: %s", granular_->GetRhythmName(),
                      granular_->GetGrainSpeedName());
            TomThumbDrawText(disp_, 0, 17, line1, true); // clear of the title's line
            // No knobs on this page, so no knob row text.
            DrawControlRow(kFooterRow1Y, false, kFooterDividerY, "", "", "", "");
            DrawControlRow(kFooterRow2Y, true, kFooterInterRowDividerY, "Rhythm", "", "",
                             "Speed");
            break;
        }
        case GranularParamPage::Scan:
        {
            char line1[28], line2[28];
            snprintf(line1, sizeof(line1), "Mode: %s", granular_->GetScanModeName());
            TomThumbDrawText(disp_, 0, 16, line1, true);
            snprintf(line2, sizeof(line2), "Direction: %s", granular_->GetDirectionName());
            TomThumbDrawText(disp_, 0, 22, line2, true);
            DrawGranularWaveformLive();

            char sc_val[10];
            FormatGranularScan(granular_, sc_val, sizeof(sc_val));
            DrawControlRow(kFooterRow1Y, false, kFooterDividerY, "Scan", sc_val, "", "");
            DrawControlRow(kFooterRow2Y, true, kFooterInterRowDividerY, "Mode", "", "",
                             "Direction");
            break;
        }
        case GranularParamPage::Loop:
        {
            // Full-height Start/End lines so the pointers' range is
            // visible directly on the waveform.
            DrawGranularWaveformLive(granular_->GetLoopStart01(), granular_->GetLoopEnd01());
            char start_val[8], end_val[8];
            snprintf(start_val, sizeof(start_val), "%d%%",
                      (int)(granular_->GetLoopStart01() * 100.f + 0.5f));
            snprintf(end_val, sizeof(end_val), "%d%%",
                      (int)(granular_->GetLoopEnd01() * 100.f + 0.5f));
            DrawControlRow(kFooterRow1Y, false, kFooterDividerY, "Start", start_val, end_val,
                             "End");
            DrawControlRow(kFooterRow2Y, true, kFooterInterRowDividerY, "", "", "", "");
            break;
        }
        case GranularParamPage::Pitch:
        {
            char line1[28];
            snprintf(line1, sizeof(line1), "Keymap: %s   %s",
                      granular_->GetGrainFollowsNote() ? "On" : "Off",
                      granular_->GetPoly() ? "Poly (4 voices)" : "Mono");
            TomThumbDrawText(disp_, 0, 16, line1, true);
            DrawGranularWaveformLive();

            char tune_val[8];
            snprintf(tune_val, sizeof(tune_val), "%+d", granular_->GetGrainTuneSemitones());
            DrawControlRow(kFooterRow1Y, false, kFooterDividerY, "Pitch", tune_val, "", "");
            DrawControlRow(kFooterRow2Y, true, kFooterInterRowDividerY,
                             granular_->GetGrainFollowsNote() ? "Keymap:On* Off" : "Keymap:On Off*",
                             "", "",
                             granular_->GetPoly() ? "Poly" : "Mono");
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
            DrawFilterCurve(granular_->GetFilterMode(), granular_->GetFilterCutoff01(),
                            granular_->GetFilterResonance01());

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
            // Same layout as every engine's Mix page: scope + output
            // meter, Level/Pan, Gain.
            DrawMixPageScope(12, granular_scope_buf_, granular_scope_capacity_, kMixGr);
            if(granular_mix_target_gain_)
            {
                char gain_val[12];
                FormatGainDb(granular_->GetGain01(), gain_val, sizeof(gain_val));
                DrawControlRow(kFooterRow1Y, false, kFooterDividerY, "Gain", gain_val, "", "");
            }
            else
            {
                char out_val[8], pan_val[8];
                snprintf(out_val, sizeof(out_val), "%d%%",
                          (int)(granular_->GetOutputLevel01() * 100.f + 0.5f));
                snprintf(pan_val, sizeof(pan_val), "%d%%",
                          (int)(granular_->GetPan01() * 100.f + 0.5f));
                DrawControlRow(kFooterRow1Y, false, kFooterDividerY, "Level", out_val, pan_val,
                                 "Pan");
            }
            DrawControlRow(kFooterRow2Y, true, kFooterInterRowDividerY,
                             granular_mix_target_gain_ ? "Level" : "Level*", "", "",
                             granular_mix_target_gain_ ? "Gain*" : "Gain");
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
                DrawControlRow(kFooterRow1Y, false, kFooterDividerY, "Send", send_val, size_val,
                                 "Size");
            }
            else
            {
                char send_val[8], time_val[12];
                snprintf(send_val, sizeof(send_val), "%d%%",
                          (int)(granular_->GetDelaySend01() * 100.f + 0.5f));
                FormatDelayTime(delay_time01_, time_val, sizeof(time_val));
                DrawControlRow(kFooterRow1Y, false, kFooterDividerY, "Send", send_val, time_val,
                                 "Time");
            }
            DrawFxSends(granular_->GetReverbSend01(), granular_->GetDelaySend01(),
                        granular_fx_target_delay_ ? 1 : 0);
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
            DrawControlRow(kFooterRow1Y, false, kFooterDividerY, "MIDI Ch", chan_val, "", "");
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
                TomThumbDrawText(disp_, 0, 16, hold_msg, true);
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
            TomThumbDrawText(disp_, 0, 16, src_line, true);

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
                        TomThumbDrawText(disp_, 0, 23 + row * 6, line, true);
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
                DrawGranularWaveformLive();
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
            // Hold-to-confirm bar for saving the capture, same look as
            // every other hold-to-save in this project.
            if(pod_->button2.Pressed())
            {
                float held = pod_->button2.TimeHeldMs();
                int   w    = (int)(Clampf(held / 800.f, 0.f, 1.f) * (disp_->Width() - 2));
                TomThumbDrawText(disp_, 0, 16, "Hold: Save Capture...", true);
                disp_->DrawRect(0, 20, disp_->Width() - 1, 24, true, false);
                if(w > 0)
                    disp_->DrawRect(1, 21, w, 23, true, true);
                DrawControlRow(kFooterRow1Y, false, kFooterDividerY, "", "", "", "");
                DrawControlRow(kFooterRow2Y, true, kFooterInterRowDividerY, "", "", "", "");
                break;
            }
            char line[32];
            if(granular_trim_status_[0] != '\0')
                snprintf(line, sizeof(line), "%s", granular_trim_status_);
            else
                snprintf(line, sizeof(line), "Start%d%% End%d%%",
                          (int)(granular_trim_start01_ * 100.f + 0.5f),
                          (int)(granular_trim_end01_ * 100.f + 0.5f));
            TomThumbDrawText(disp_, 0, 16, line, true);

            // Reuses DrawGranularWaveform()'s two-marker drawing as-is --
            // here the markers are the trim start/end points rather than
            // Grain/Scan anchors, over the FULL untrimmed capture (see
            // granular_trim_full_peaks_'s own comment), not whatever
            // sub-range is currently active.
            DrawGranularWaveform(granular_trim_full_peaks_, granular_trim_start01_,
                                  &granular_trim_end01_, 1, granular_capture_full_len_ > 0);

            char s_val[8], e_val[8];
            snprintf(s_val, sizeof(s_val), "%d%%", (int)(granular_trim_start01_ * 100.f + 0.5f));
            snprintf(e_val, sizeof(e_val), "%d%%", (int)(granular_trim_end01_ * 100.f + 0.5f));
            DrawControlRow(kFooterRow1Y, false, kFooterDividerY, "Start", s_val, e_val, "End");
            DrawControlRow(kFooterRow2Y, true, kFooterInterRowDividerY, "Play", "", "",
                             granular_capture_full_len_ > 0 ? "Hold=Save" : "");
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
            if(granular_lib_dirty_)
            {
                granular_captured_bank_count_ = PerformanceStore::ListGranularCapturedBanks(
                    granular_captured_banks_, PerformanceStore::kMaxGranularCapturedBanks);
                int n = 1 + granular_captured_bank_count_;
                if(granular_lib_folder_cursor_ >= n)
                    granular_lib_folder_cursor_ = 0;
                if(granular_lib_folder_ >= n)
                    granular_lib_folder_ = -1; // that folder's gone -- back to the list
                granular_lib_dirty_ = false;
            }
            bool at_folder_list = granular_lib_folder_ < 0;
            char folder_name[16], folder_label[24];
            if(!at_folder_list && granular_lib_items_dirty_)
            {
                GranularLibFolder(granular_lib_folder_, folder_name, sizeof(folder_name),
                                  folder_label, sizeof(folder_label));
                granular_lib_count_ = PerformanceStore::ListGranularLibraryItems(
                    folder_name, granular_lib_names_, kGranularLibListMax);
                if(granular_lib_cursor_ >= granular_lib_count_)
                    granular_lib_cursor_ = 0;
                granular_lib_items_dirty_ = false;
            }

            bool browsing_load = save_load_mode_ == SaveLoadMode::BrowsingLoad;
            bool in_folder     = browsing_load && !at_folder_list;

            if(!at_folder_list)
                GranularLibFolder(granular_lib_folder_, folder_name, sizeof(folder_name),
                                  folder_label, sizeof(folder_label));
            TomThumbDrawText(disp_, 0, 16, at_folder_list ? "Starter+Captured" : folder_label,
                             true);

            if(browsing_load)
            {
                char line2[48];
                if(at_folder_list)
                    GranularLibFolder(granular_lib_folder_cursor_, folder_name,
                                      sizeof(folder_name), line2, sizeof(line2));
                else if(granular_lib_count_ > 0)
                {
                    // "> " marks the sound that's loaded right now -- you
                    // stay in the folder after loading, so it's worth showing.
                    const char* nm     = granular_lib_names_[granular_lib_cursor_];
                    bool        loaded = strcmp(granular_loaded_lib_folder_, folder_name) == 0
                                  && strcmp(granular_loaded_lib_name_, nm) == 0;
                    char shown[61];
                    GranularLibDisplayName(nm, shown, sizeof(shown));
                    snprintf(line2, sizeof(line2), "%s%s", loaded ? "> " : "", shown);
                }
                else
                    snprintf(line2, sizeof(line2), "(empty)");
                disp_->SetCursor(0, 20);
                WriteUpper(line2);
            }
            else
            {
                char line1[48];
                if(granular_loaded_lib_name_[0] != '\0')
                {
                    char shown[61];
                    GranularLibDisplayName(granular_loaded_lib_name_, shown, sizeof(shown));
                    snprintf(line1, sizeof(line1), "Now: %s", shown);
                }
                else if(granular_loaded_preset_slot_ > 0)
                    snprintf(line1, sizeof(line1), "Now: GRAINS%d", granular_loaded_preset_slot_);
                else
                    snprintf(line1, sizeof(line1), "Now: (new)");
                disp_->SetCursor(0, 20);
                WriteUpper(line1);
                if(granular_preset_status_[0] != '\0')
                {
                    disp_->SetCursor(0, 32);
                    WriteUpper(granular_preset_status_);
                }
            }

            // Shared browse convention: K1 scrolls folders, K2 scrolls the
            // sounds inside one, B1 backs out a level, B2 opens/loads.
            DrawControlRow(kFooterRow1Y, false, kFooterDividerY,
                             browsing_load && at_folder_list ? "Scroll" : "", "", "",
                             in_folder ? "Scroll" : "");
            DrawControlRow(kFooterRow2Y, true, kFooterInterRowDividerY,
                             save_load_mode_ == SaveLoadMode::Idle ? "" : "Back", "", "",
                             save_load_mode_ == SaveLoadMode::Idle
                                 ? "Load"
                                 : (at_folder_list ? "Open"
                                                   : (granular_lib_count_ > 0 ? "Load" : "")));
            break;
        }
        case GranularParamPage::User:
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

            int  loaded        = granular_loaded_preset_slot_;
            bool choosing_save = save_load_mode_ == SaveLoadMode::ChoosingSave;
            bool browsing_load = save_load_mode_ == SaveLoadMode::BrowsingLoad;

            // Saving keeps its hold-to-confirm bar; loading is a plain tap.
            if(pod_->button2.Pressed() && choosing_save)
            {
                float held = pod_->button2.TimeHeldMs();
                int   w    = (int)(Clampf(held / 800.f, 0.f, 1.f) * (disp_->Width() - 2));
                disp_->SetCursor(0, 20);
                WriteUpper("Hold: Save...");
                disp_->DrawRect(0, 30, disp_->Width() - 1, 34, true, false);
                if(w > 0)
                    disp_->DrawRect(1, 31, w, 33, true, true);
                DrawControlRow(kFooterRow1Y, false, kFooterDividerY, "", "", "", "");
                DrawControlRow(kFooterRow2Y, true, kFooterInterRowDividerY, "", "", "", "");
                break;
            }

            TomThumbDrawText(disp_, 0, 16, "User Presets", true);

            if(choosing_save)
            {
                char line1[24], line2[16];
                snprintf(line1, sizeof(line1), "%c Overwrite%s", !save_as_new_ ? '>' : ' ',
                          loaded > 0 ? "" : " (n/a)");
                snprintf(line2, sizeof(line2), "%c Save New", save_as_new_ ? '>' : ' ');
                disp_->SetCursor(0, 24);
                WriteUpper(line1);
                disp_->SetCursor(0, 34);
                WriteUpper(line2);
            }
            else if(browsing_load)
            {
                // "GRAINS" matches Global:SdMgmt's own short code for user saves.
                char line[24];
                if(granular_user_cursor_ == 0)
                    snprintf(line, sizeof(line), "New (blank)");
                else
                {
                    int slot = granular_preset_user_slots_[granular_user_cursor_ - 1];
                    snprintf(line, sizeof(line), "%sGRAINS%d", slot == loaded ? "> " : "", slot);
                }
                disp_->SetCursor(0, 24);
                WriteUpper(line);
            }
            else
            {
                char line1[48];
                if(loaded > 0)
                    snprintf(line1, sizeof(line1), "Now: GRAINS%d", loaded);
                else if(granular_loaded_lib_name_[0] != '\0')
                {
                    char shown[61];
                    GranularLibDisplayName(granular_loaded_lib_name_, shown, sizeof(shown));
                    snprintf(line1, sizeof(line1), "Now: %s", shown);
                }
                else
                    snprintf(line1, sizeof(line1), "Now: (new)");
                disp_->SetCursor(0, 24);
                WriteUpper(line1);
                if(granular_preset_status_[0] != '\0')
                {
                    disp_->SetCursor(0, 36);
                    WriteUpper(granular_preset_status_);
                }
            }

            DrawControlRow(kFooterRow1Y, false, kFooterDividerY, choosing_save ? "Select" : "",
                             "", "", browsing_load ? "Scroll" : "");
            DrawControlRow(kFooterRow2Y, true, kFooterInterRowDividerY,
                             save_load_mode_ == SaveLoadMode::Idle ? "Save" : "Back", "", "",
                             choosing_save ? "Hold=Save" : "Load");
            break;
        }
        default: break;
    }
}

int Ui::MixerEngineChannel(int seq_engine)
{
    // MidiSequencer::Engine order.
    static const int kIds[MidiSequencer::kNumEngines]
        = {kMixGr, kMixDxa, kMixDxb, kMixEns, kMixOrg, kMixSmv, kMixSmd, kMixSsw};
    return seq_engine >= 0 && seq_engine < MidiSequencer::kNumEngines ? kIds[seq_engine] : kMixGr;
}

int Ui::MixerSetChannel(int pos) const
{
    if(mixer_set_ == 1) // Global:Synths
    {
        // The synths (the Supersaw beside the Ensemble and Organ), then
        // the samplers.
        static const int kSynths[kMixSynthsCount] = {kMixGr,  kMixDxa, kMixDxb, kMixEns, kMixOrg,
                                                     kMixSsw, kMixSmv, kMixSmd, kMixByp, kMixMas};
        return kSynths[pos < 0 ? 0 : (pos >= kMixSynthsCount ? kMixSynthsCount - 1 : pos)];
    }
    // Global:Tracks: L1-L4, M1-M8 (each its engine's channel), Bypass, Master.
    if(pos < 4)
        return kMixL1 + (pos < 0 ? 0 : pos);
    if(pos < 12)
        return MixerEngineChannel(seq_ ? seq_->GetEngine(pos - 4) : 0);
    return pos == 12 ? kMixByp : kMixMas;
}

const char* Ui::MixerSetName(int pos, bool engine) const
{
    if(mixer_set_ == 0 && pos >= 4 && pos < 12)
    {
        static char buf[12];
        if(engine)
            snprintf(buf, sizeof(buf), "M%d>%s", pos - 3,
                     MixerChannelName(MixerSetChannel(pos)));
        else
            snprintf(buf, sizeof(buf), "M%d", pos - 3);
        return buf;
    }
    return MixerChannelName(MixerSetChannel(pos));
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
        case 5: return "DXA";
        case 6: return "DXB";
        case 7: return "ESB";
        case 8: return "ORG";
        case 9: return "VOI";
        case 10: return "DRM";
        case 11: return "SSW";
        case 12: return "BYP";
        case 13: return "MAS";
        default: return "?";
    }
}

float Ui::MixerGetVolume01(int ch) const
{
    switch(ch)
    {
        case 0: case 1: case 2: case 3: return layers_[ch].GetVolume01();
        case 4: return granular_ ? granular_->GetOutputLevel01() : 0.f;
        case 5: return dexed_engines_[0] ? dexed_engines_[0]->GetEngineLevel01() : 0.f;
        case 6: return dexed_engines_[1] ? dexed_engines_[1]->GetEngineLevel01() : 0.f;
        case 7: return ensemble_engines_[0] ? ensemble_engines_[0]->GetOutputLevel01() : 0.f;
        case 8: return ensemble_engines_[1] ? ensemble_engines_[1]->GetOutputLevel01() : 0.f;
        case 9: return sample_synth_engines_[0] ? sample_synth_engines_[0]->GetOutputLevel01() : 0.f;
        case 10: return sample_synth_engines_[1] ? sample_synth_engines_[1]->GetOutputLevel01() : 0.f;
        case 11: return supersaw_ ? supersaw_->GetOutputLevel01() : 0.f;
        case 12: return bypass_mix_volume01_;
        case 13: return master_volume01_;
        default: return 0.f;
    }
}

float Ui::MixerGetPan01(int ch) const
{
    switch(ch)
    {
        case 0: case 1: case 2: case 3: return layers_[ch].GetPan01();
        case 4: return granular_ ? granular_->GetPan01() : 0.5f;
        case 5: return dexed_engines_[0] ? dexed_engines_[0]->GetEnginePan01() : 0.5f;
        case 6: return dexed_engines_[1] ? dexed_engines_[1]->GetEnginePan01() : 0.5f;
        case 7: return ensemble_engines_[0] ? ensemble_engines_[0]->GetPan01() : 0.5f;
        case 8: return ensemble_engines_[1] ? ensemble_engines_[1]->GetPan01() : 0.5f;
        case 9: return sample_synth_engines_[0] ? sample_synth_engines_[0]->GetPan01() : 0.5f;
        case 10: return sample_synth_engines_[1] ? sample_synth_engines_[1]->GetPan01() : 0.5f;
        case 11: return supersaw_ ? supersaw_->GetPan01() : 0.5f;
        case 12: return bypass_pan01_;
        default: return 0.5f; // Master has none
    }
}

float Ui::MixerGetSend01(int ch) const
{
    switch(ch)
    {
        case 0: case 1: case 2: case 3: return layers_[ch].GetReverbSend01();
        case 4: return granular_ ? granular_->GetReverbSend01() : 0.f;
        case 5: // the two engines share the Dexed bus's sends
        case 6: return dexed_ ? dexed_->GetReverbSend01() : 0.f;
        case 7: return ensemble_engines_[0] ? ensemble_engines_[0]->GetReverbSend01() : 0.f;
        case 8: return ensemble_engines_[1] ? ensemble_engines_[1]->GetReverbSend01() : 0.f;
        case 9: return sample_synth_engines_[0] ? sample_synth_engines_[0]->GetReverbSend01() : 0.f;
        case 10: return sample_synth_engines_[1] ? sample_synth_engines_[1]->GetReverbSend01() : 0.f;
        case 11: return supersaw_ ? supersaw_->GetReverbSend01() : 0.f;
        case 12: return bypass_reverb_send01_;
        default: return 0.f; // Master uses Reverb Size instead (see reverb_size01_)
    }
}

float Ui::MixerGetDelaySend01(int ch) const
{
    switch(ch)
    {
        case 0: case 1: case 2: case 3: return layers_[ch].GetDelaySend01();
        case 4: return granular_ ? granular_->GetDelaySend01() : 0.f;
        case 5:
        case 6: return dexed_ ? dexed_->GetDelaySend01() : 0.f;
        case 7: return ensemble_engines_[0] ? ensemble_engines_[0]->GetDelaySend01() : 0.f;
        case 8: return ensemble_engines_[1] ? ensemble_engines_[1]->GetDelaySend01() : 0.f;
        case 9: return sample_synth_engines_[0] ? sample_synth_engines_[0]->GetDelaySend01() : 0.f;
        case 10: return sample_synth_engines_[1] ? sample_synth_engines_[1]->GetDelaySend01() : 0.f;
        case 11: return supersaw_ ? supersaw_->GetDelaySend01() : 0.f;
        case 12: return bypass_delay_send01_;
        default: return 0.f;
    }
}

void Ui::MixerSetDelaySend01(int ch, float v01)
{
    switch(ch)
    {
        case 0: case 1: case 2: case 3: layers_[ch].SetDelaySend01(v01); break;
        case 4: if(granular_) granular_->SetDelaySend01(v01); break;
        case 5:
        case 6: if(dexed_) dexed_->SetDelaySend01(v01); break;
        case 7: if(ensemble_engines_[0]) ensemble_engines_[0]->SetDelaySend01(v01); break;
        case 8: if(ensemble_engines_[1]) ensemble_engines_[1]->SetDelaySend01(v01); break;
        case 9: if(sample_synth_engines_[0]) sample_synth_engines_[0]->SetDelaySend01(v01); break;
        case 10: if(sample_synth_engines_[1]) sample_synth_engines_[1]->SetDelaySend01(v01); break;
        case 11: if(supersaw_) supersaw_->SetDelaySend01(Clampf(v01, 0.f, 1.f)); break;
        case 12: bypass_delay_send01_ = Clampf(v01, 0.f, 1.f); break;
        default: break;
    }
}

void Ui::MixerSetVolume01(int ch, float v01)
{
    switch(ch)
    {
        case 0: case 1: case 2: case 3: layers_[ch].SetVolume01(v01); break;
        case 4: if(granular_) granular_->SetOutputLevel01(v01); break;
        case 5: if(dexed_engines_[0]) dexed_engines_[0]->SetEngineLevel01(v01); break;
        case 6: if(dexed_engines_[1]) dexed_engines_[1]->SetEngineLevel01(v01); break;
        case 7: if(ensemble_engines_[0]) ensemble_engines_[0]->SetOutputLevel01(v01); break;
        case 8: if(ensemble_engines_[1]) ensemble_engines_[1]->SetOutputLevel01(v01); break;
        case 9: if(sample_synth_engines_[0]) sample_synth_engines_[0]->SetOutputLevel01(v01); break;
        case 10: if(sample_synth_engines_[1]) sample_synth_engines_[1]->SetOutputLevel01(v01); break;
        case 11: if(supersaw_) supersaw_->SetOutputLevel01(v01); break;
        case 12: SetBypassMixVolume01(v01); break;
        case 13:
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
        case 5: if(dexed_engines_[0]) dexed_engines_[0]->SetEnginePan01(v01); break;
        case 6: if(dexed_engines_[1]) dexed_engines_[1]->SetEnginePan01(v01); break;
        case 7: if(ensemble_engines_[0]) ensemble_engines_[0]->SetPan01(v01); break;
        case 8: if(ensemble_engines_[1]) ensemble_engines_[1]->SetPan01(v01); break;
        case 9: if(sample_synth_engines_[0]) sample_synth_engines_[0]->SetPan01(v01); break;
        case 10: if(sample_synth_engines_[1]) sample_synth_engines_[1]->SetPan01(v01); break;
        case 11: if(supersaw_) supersaw_->SetPan01(v01); break;
        case 12: SetBypassPan01(v01); break;
        default: break; // Master has no Pan -- no-op
    }
}

void Ui::MixerSetSend01(int ch, float v01)
{
    switch(ch)
    {
        case 0: case 1: case 2: case 3: layers_[ch].SetReverbSend01(v01); break;
        case 4: if(granular_) granular_->SetReverbSend01(v01); break;
        case 5:
        case 6: if(dexed_) dexed_->SetReverbSend01(v01); break;
        case 7: if(ensemble_engines_[0]) ensemble_engines_[0]->SetReverbSend01(v01); break;
        case 8: if(ensemble_engines_[1]) ensemble_engines_[1]->SetReverbSend01(v01); break;
        case 9: if(sample_synth_engines_[0]) sample_synth_engines_[0]->SetReverbSend01(v01); break;
        case 10: if(sample_synth_engines_[1]) sample_synth_engines_[1]->SetReverbSend01(v01); break;
        case 11: if(supersaw_) supersaw_->SetReverbSend01(Clampf(v01, 0.f, 1.f)); break;
        case 12: bypass_reverb_send01_ = Clampf(v01, 0.f, 1.f); break;
        default: break; // Master uses Reverb Size instead, set directly in ApplyKnobs()
    }
}

bool Ui::MixerChannelHasPan(int ch) const
{
    // Only Master lacks a Pan concept now -- Dexed got a real Pan
    // control after a user report that its live output, having no pan
    // stage at all, reached the mix disproportionately loud next to
    // every other source's own center-pan-attenuated signal.
    return ch != kMixMas;
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
        case 5: // the shared bus's meter
        case 6: return dexed_ ? dexed_->GetPreLimiterLevel01() : 0.f;
        case 7: return ensemble_engines_[0] ? ensemble_engines_[0]->GetPreLimiterLevel01() : 0.f;
        case 8: return ensemble_engines_[1] ? ensemble_engines_[1]->GetPreLimiterLevel01() : 0.f;
        case 9: return sample_synth_engines_[0] ? sample_synth_engines_[0]->GetPreLimiterLevel01() : 0.f;
        case 10: return sample_synth_engines_[1] ? sample_synth_engines_[1]->GetPreLimiterLevel01() : 0.f;
        case 11: return supersaw_ ? supersaw_->GetPreLimiterLevel01() : 0.f;
        case 12: return g_bypass_level;
        case 13: return g_master_level;
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
        case 5:
        case 6: return dexed_ && dexed_->IsLimiting();
        case 7: return ensemble_engines_[0] && ensemble_engines_[0]->IsLimiting();
        case 8: return ensemble_engines_[1] && ensemble_engines_[1]->IsLimiting();
        case 9: return sample_synth_engines_[0] && sample_synth_engines_[0]->IsLimiting();
        case 10: return sample_synth_engines_[1] && sample_synth_engines_[1]->IsLimiting();
        case 11: return supersaw_ && supersaw_->IsLimiting();
        case 12: return g_bypass_clip_hold > 0.f;
        case 13: return g_master_clip_hold > 0.f;
        default: return false;
    }
}

// Vertical fader-style bar -- (x0,y0) is the box's top-left corner, fills
// from the BOTTOM up (unlike every other bar in this project, which fills
// left-to-right -- see the Mixer design discussion this was built from
// for why a vertical fader reads better for a per-channel level here).
float Ui::MeterBar01(float level)
{
    // Peak level -> bar height on a dB scale, like a studio meter:
    // -48 dB at the bottom to 0 dBFS at the top, so a healthy -6 dB sits
    // near the top (it used to be a linear scale, where -6 dB looked
    // "half way" and normal levels read as quiet).
    constexpr float kFloorDb = -48.f;
    if(level <= 0.f)
        return 0.f;
    float db = 20.f * log10f(level);
    return db <= kFloorDb ? 0.f : (db >= 0.f ? 1.f : 1.f - db / kFloorDb);
}

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

void Ui::DrawMixerHBar(int x0, int y0, int w, int h, float v01, bool clip_lit)
{
    int x1 = x0 + w - 1;
    int y1 = y0 + h - 1;
    disp_->DrawRect(x0, y0, x1, y1, true, false);
    int fill_w = (int)(Clampf(v01, 0.f, 1.f) * (float)(w - 2) + 0.5f);
    if(fill_w > 0)
        disp_->DrawRect(x0 + 1, y0 + 1, x0 + fill_w, y1 - 1, true, true);
    // Clip latch, same idea as DrawMixerVBar()'s cap -- drawn last so
    // the fill never hides it.
    if(clip_lit)
        disp_->DrawRect(x1 - 2, y0 + 1, x1 - 1, y1 - 1, true, true);
}

void Ui::DrawMixerOverviewGrid(int top_y)
{
    // Live peak level, not the Volume fader's own setting (see
    // MixerGetLevel01()'s own comment): this grid answers "what's
    // loud/clipping right now". Two rows of cells, each a name centred
    // over a horizontal meter: Tracks' 14 in rows of 7, Synths' 10 in
    // rows of 5. The second row's meters end at y=36, clear of the
    // "PUSH ENC TO OPEN" caption beneath.
    const int n     = MixerSetCount();
    const int kCols = n > 10 ? 7 : 5, kCellW = 126 / kCols, kBarW = kCellW - 2;
    constexpr int kBarH = 5, kRowH = 14;
    for(int pos = 0; pos < n; pos++)
    {
        const int   ch   = MixerSetChannel(pos);
        const int   x    = 1 + (pos % kCols) * kCellW;
        const int   base = top_y + (pos / kCols) * kRowH;
        const char* name = MixerSetName(pos);
        TomThumbDrawText(disp_, x + (kBarW - TomThumbInkWidth(name)) / 2, base, name, true);
        DrawMixerHBar(x, base + 2, kBarW, kBarH, MeterBar01(MixerGetLevel01(ch)),
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
        case DexedParamPage::Split: page_name = "Split"; break;
        case DexedParamPage::Advanced: page_name = "Advanced"; break;
        case DexedParamPage::Preset: page_name = "Preset"; break;
        case DexedParamPage::User: page_name = "User"; break;
        case DexedParamPage::Import: page_name = "Import"; break;
        default: break;
    }
    char title[24];
    snprintf(title, sizeof(title), "DXD-%c:%s", dexed_active_ == 1 ? 'B' : 'A', page_name);
    DrawPageTitle(title, DexedUnsaved());
    DrawBeatIndicator(disp_->Width() - 41, 0, 3);
    disp_->DrawLine(0, 9, disp_->Width() - 1, 9, true);

    switch(dexed_param_page_)
    {
        case DexedParamPage::Algo:
        {
            int algo0 = dexed_->GetPatchByte(134);
            // View-only: ALGORITHM is chosen on Advanced's first page.
            DrawAlgorithmDiagram(algo0);
            DrawControlRow(kFooterRow1Y, false, kFooterDividerY, "", "", "", "");
            DrawControlRow(kFooterRow2Y, true, kFooterInterRowDividerY, "", "", "", "");
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
            TomThumbDrawText(disp_, 0, 16, "50% = as saved", true);
            DrawOscilloscope(18, kFooterDividerY - 2, dexed_scope_buf_, dexed_scope_capacity_);
            DrawControlRow(kFooterRow1Y, false, kFooterDividerY, "Amt", val, "", "");
            DrawControlRow(kFooterRow2Y, true, kFooterInterRowDividerY, "", "", "", "");
            break;
        }
        case DexedParamPage::EnvSpeed:
        {
            char val[8];
            snprintf(val, sizeof(val), "%d%%", (int)(dexed_->GetEnvSpeed01() * 100.f + 0.5f));
            TomThumbDrawText(disp_, 0, 16, "50% = as saved", true);
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
            DrawFilterCurve(dexed_->GetFilterMode(), dexed_->GetFilterCutoff01(),
                            dexed_->GetFilterResonance01());

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
            // Three bars: each engine's own Level/Pan into the bus, then
            // the bus itself. B1 picks which one K1/K2 set; B2 puts K1 on
            // the bus Gain. Reverb Send lives on the FX page (and
            // Global:Mixer).
            const int sel = dexed_mix_target_gain_ ? -1 : dexed_mix_sel_;
            DexedSynth* a = dexed_engines_[0];
            DexedSynth* b = dexed_engines_[1];
            DrawDexedMixColumn(16, "DXD-A", sel == 0, a->GetEngineLevel01(), a->GetEnginePan01());
            DrawDexedMixColumn(48, "DXD-B", sel == 1, b->GetEngineLevel01(), b->GetEnginePan01());
            DrawDexedMixColumn(80, "BUS", sel == 2, dexed_->GetOutputLevel01(), dexed_->GetPan01());
            // Fourth bar: the live output meter every engine's Mix page
            // has, with its clip latch -- the same reading as the DXD
            // channel on Global:Mixer.
            {
                const int cx = 112;
                int       nw = TomThumbAdvanceWidth("OUT");
                TomThumbDrawText(disp_, cx - nw / 2, 17, "OUT", true);
                DrawMixerVBar(cx - 6, 20, 13, 17, MeterBar01(MixerGetLevel01(kMixDxa)), MixerIsClipping(kMixDxa));
            }
            if(dexed_mix_target_gain_)
            {
                char gain_val[12];
                FormatGainDb(dexed_->GetGain01(), gain_val, sizeof(gain_val));
                DrawControlRow(kFooterRow1Y, false, kFooterDividerY, "Gain", gain_val, "", "");
            }
            else
            {
                float level01 = sel == 2 ? dexed_->GetOutputLevel01()
                                         : dexed_engines_[sel]->GetEngineLevel01();
                float pan01   = sel == 2 ? dexed_->GetPan01() : dexed_engines_[sel]->GetEnginePan01();
                char  out_val[8], pan_val[8];
                snprintf(out_val, sizeof(out_val), "%d%%", (int)(level01 * 100.f + 0.5f));
                FormatPan(pan01, pan_val, sizeof(pan_val));
                DrawControlRow(kFooterRow1Y, false, kFooterDividerY, "Level", out_val, pan_val,
                                 "Pan");
            }
            static const char* const kSel[3] = {"A* B Bus", "A B* Bus", "A B Bus*"};
            DrawControlRow(kFooterRow2Y, true, kFooterInterRowDividerY,
                             sel < 0 ? "A B Bus" : kSel[sel], "", "",
                             dexed_mix_target_gain_ ? "Gain*" : "Gain");
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
                DrawControlRow(kFooterRow1Y, false, kFooterDividerY, "Send", send_val, size_val,
                                 "Size");
            }
            else
            {
                char send_val[8], time_val[12];
                snprintf(send_val, sizeof(send_val), "%d%%",
                          (int)(dexed_->GetDelaySend01() * 100.f + 0.5f));
                FormatDelayTime(delay_time01_, time_val, sizeof(time_val));
                DrawControlRow(kFooterRow1Y, false, kFooterDividerY, "Send", send_val, time_val,
                                 "Time");
            }
            DrawFxSends(dexed_->GetReverbSend01(), dexed_->GetDelaySend01(), dexed_fx_target_delay_ ? 1 : 0);
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
            auto chan_text = [](int ch, char* out, size_t n) {
                if(ch < 0)
                    snprintf(out, n, "Omni");
                else
                    snprintf(out, n, "%d", ch + 1);
            };
            char chan_val[8];
            chan_text(DexedActiveChannel(), chan_val, sizeof(chan_val));
            char voices_val[8];
            snprintf(voices_val, sizeof(voices_val), "%d", dexed_->GetMaxVoices());
            // Who listens to what, above the scope.
            char line[32];
            if(dexed_midi_lock_)
            {
                snprintf(line, sizeof(line), "A + B LOCKED: %s%s",
                          dexed_midi_channel_ < 0 ? "" : "CH ", chan_val);
                TomThumbDrawText(disp_, 1, 17, line, true);
            }
            else
            {
                char a_ch[8], b_ch[8];
                chan_text(dexed_midi_channel_, a_ch, sizeof(a_ch));
                chan_text(dexed_b_midi_channel_, b_ch, sizeof(b_ch));
                snprintf(line, sizeof(line), "A: %s%s", dexed_midi_channel_ < 0 ? "" : "CH ", a_ch);
                TomThumbDrawText(disp_, 1, 17, line, true);
                snprintf(line, sizeof(line), "B: %s%s", dexed_b_midi_channel_ < 0 ? "" : "CH ",
                          b_ch);
                TomThumbDrawText(disp_, 64, 17, line, true);
            }
            DrawOscilloscope(20, kFooterDividerY - 2, dexed_scope_buf_, dexed_scope_capacity_);
            DrawControlRow(kFooterRow1Y, false, kFooterDividerY, "MIDI Ch", chan_val, voices_val,
                             "Voices");
            DrawControlRow(kFooterRow2Y, true, kFooterInterRowDividerY,
                             dexed_midi_lock_ ? "Lock:On* Off" : "Lock:On Off*", "", "", "");
            break;
        }
        case DexedParamPage::Split: DrawDexedSplitPage(); break;
        case DexedParamPage::Advanced:
        {
            // Entry point into Screen::DexedOperator -- same "Click to
            // open..." idiom as GlobalPage::Granular/Dexed's own entry
            // pages, just reached by a click on this page instead of a
            // Global one.
            disp_->SetCursor(0, 20);
            WriteUpper("Full DX7 voice edit");
            disp_->SetCursor(0, 30);
            WriteUpper("Click to open");
            DrawControlRow(kFooterRow1Y, false, kFooterDividerY, "", "", "", "");
            DrawControlRow(kFooterRow2Y, true, kFooterInterRowDividerY, "", "", "", "");
            break;
        }
        case DexedParamPage::Preset:
        {
            // No card check here: Roms and Dexed are built into the
            // firmware, only the Imports group lives on the card (and
            // shows "(no card)" itself when it's missing).
            if(dexed_preset_slots_dirty_)
                RefreshDexedPresetSlots();

            bool browsing_load = save_load_mode_ == SaveLoadMode::BrowsingLoad;
            bool at_groups     = browsing_load && !dexed_files_group_open_;
            bool at_folders    = browsing_load && dexed_files_group_open_ && !dexed_preset_folder_open_;
            bool in_folder     = browsing_load && dexed_files_group_open_ && dexed_preset_folder_open_;

            // Same layout as every other engine's browser: a Tom Thumb
            // header naming the level, then the one highlighted entry.
            static const char* const kGroups[] = {"Roms", "Dexed", "Imports"};
            if(at_groups)
            {
                TomThumbDrawText(disp_, 0, 16, "Roms+Dexed+Imports", true);
                disp_->SetCursor(0, 20);
                WriteUpper(kGroups[(int)dexed_files_group_]);
            }
            else if(at_folders || in_folder)
            {
                // Folder name + size; inside a folder, the preset below it.
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
                        name  = PerformanceStore::IsCardPresent() ? "(no imports)" : "(no card)";
                        count = 0;
                    }
                }
                else
                {
                    int cat = ResolveDexedRealCategory();
                    name    = DexedSynth::GetFactoryCategoryName(cat);
                    count   = DexedSynth::GetFactoryCategoryCount(cat);
                }
                char folder_line[32];
                if(count > 0)
                    snprintf(folder_line, sizeof(folder_line), "%s (%d)", name, count);
                else
                    snprintf(folder_line, sizeof(folder_line), "%s", name);
                if(at_folders)
                {
                    TomThumbDrawText(disp_, 0, 16, kGroups[(int)dexed_files_group_], true);
                    disp_->SetCursor(0, 20);
                    WriteUpper(folder_line);
                }
                else
                {
                    TomThumbDrawText(disp_, 0, 16, folder_line, true);
                    int  slot = ResolveDexedPresetSlot();
                    char line2[24];
                    if(slot < 0)
                        snprintf(line2, sizeof(line2), "(empty)");
                    else
                        // "> " marks the preset that's loaded right now --
                        // you stay in the folder after loading.
                        snprintf(line2, sizeof(line2), "%s%s",
                                  slot == dexed_loaded_preset_slot_ ? "> " : "",
                                  DexedSlotName(slot));
                    disp_->SetCursor(0, 20);
                    WriteUpper(line2);
                }
            }
            else
            {
                char line1[32];
                snprintf(line1, sizeof(line1), "Now: %s", DexedSlotName(dexed_loaded_preset_slot_));
                disp_->SetCursor(0, 20);
                WriteUpper(line1);
                if(dexed_preset_status_[0] != '\0')
                {
                    disp_->SetCursor(0, 32);
                    WriteUpper(dexed_preset_status_);
                }
            }

            // Shared browse convention: K1 scrolls groups/folders, K2 the
            // presets inside one, B1 backs out a level, B2 opens/loads.
            DrawControlRow(kFooterRow1Y, false, kFooterDividerY,
                             (at_groups || at_folders) ? "Scroll" : "", "", "",
                             in_folder ? "Scroll" : "");
            DrawControlRow(kFooterRow2Y, true, kFooterInterRowDividerY,
                             browsing_load ? "Back" : "", "", "",
                             !browsing_load ? "Load" : (in_folder ? "Load" : "Open"));
            break;
        }
        case DexedParamPage::User:
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

            int  loaded        = dexed_loaded_preset_slot_;
            bool choosing_save = save_load_mode_ == SaveLoadMode::ChoosingSave;
            bool browsing_load = save_load_mode_ == SaveLoadMode::BrowsingLoad;

            // Saving keeps its hold-to-confirm bar; loading is a plain tap.
            if(pod_->button2.Pressed() && choosing_save)
            {
                float held = pod_->button2.TimeHeldMs();
                int   w    = (int)(Clampf(held / 800.f, 0.f, 1.f) * (disp_->Width() - 2));
                disp_->SetCursor(0, 20);
                WriteUpper("Hold: Save...");
                disp_->DrawRect(0, 30, disp_->Width() - 1, 34, true, false);
                if(w > 0)
                    disp_->DrawRect(1, 31, w, 33, true, true);
                DrawControlRow(kFooterRow1Y, false, kFooterDividerY, "", "", "", "");
                DrawControlRow(kFooterRow2Y, true, kFooterInterRowDividerY, "", "", "", "");
                break;
            }

            TomThumbDrawText(disp_, 0, 16, "User Presets", true);

            if(choosing_save)
            {
                bool can_overwrite = loaded > DexedSynth::GetNumFactoryPresets();
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
                char line[24];
                if(dexed_user_cursor_ == 0)
                    snprintf(line, sizeof(line), "New");
                else
                {
                    int slot = dexed_preset_user_slots_[dexed_user_cursor_ - 1];
                    snprintf(line, sizeof(line), "%s%s", slot == loaded ? "> " : "",
                              DexedSlotName(slot));
                }
                disp_->SetCursor(0, 24);
                WriteUpper(line);
            }
            else
            {
                char line1[32];
                snprintf(line1, sizeof(line1), "Now: %s", DexedSlotName(loaded));
                disp_->SetCursor(0, 24);
                WriteUpper(line1);
                if(dexed_preset_status_[0] != '\0')
                {
                    disp_->SetCursor(0, 36);
                    WriteUpper(dexed_preset_status_);
                }
            }

            DrawControlRow(kFooterRow1Y, false, kFooterDividerY, choosing_save ? "Select" : "",
                             "", "", browsing_load ? "Scroll" : "");
            DrawControlRow(kFooterRow2Y, true, kFooterInterRowDividerY,
                             save_load_mode_ == SaveLoadMode::Idle ? "Save" : "Back", "", "",
                             choosing_save ? "Hold=Save" : "Load");
            break;
        }
        case DexedParamPage::Import:
        {
            if(!PerformanceStore::IsCardPresent())
            {
                disp_->SetCursor(0, 20);
                WriteUpper("No card");
                DrawControlRow(kFooterRow1Y, false, kFooterDividerY, "", "", "", "");
                DrawControlRow(kFooterRow2Y, true, kFooterInterRowDividerY, "", "", "", "Retry");
                break;
            }
            if(dexed_import_files_dirty_)
                RefreshDexedImportFiles();
            if(pod_->button2.Pressed() && dexed_import_file_count_ > 0)
            {
                float held = pod_->button2.TimeHeldMs();
                int   w    = (int)(Clampf(held / 800.f, 0.f, 1.f) * (disp_->Width() - 2));
                TomThumbDrawText(disp_, 0, 16, "Hold: Import...", true);
                disp_->DrawRect(0, 20, disp_->Width() - 1, 24, true, false);
                if(w > 0)
                    disp_->DrawRect(1, 21, w, 23, true, true);
                DrawControlRow(kFooterRow1Y, false, kFooterDividerY, "", "", "", "");
                DrawControlRow(kFooterRow2Y, true, kFooterInterRowDividerY, "", "", "", "");
                break;
            }

            TomThumbDrawText(disp_, 0, 16, "From DEXED IMPORT/", true);
            char line[48];
            if(dexed_import_file_count_ <= 0)
                snprintf(line, sizeof(line), "(no .syx files)");
            else
                snprintf(line, sizeof(line), "%s", dexed_import_names_[dexed_import_cursor_]);
            disp_->SetCursor(0, 24);
            WriteUpper(line);
            if(dexed_preset_status_[0] != '\0')
            {
                disp_->SetCursor(0, 36);
                WriteUpper(dexed_preset_status_);
            }
            DrawControlRow(kFooterRow1Y, false, kFooterDividerY, "", "", "",
                             dexed_import_file_count_ > 0 ? "Scroll" : "");
            DrawControlRow(kFooterRow2Y, true, kFooterInterRowDividerY, "", "", "",
                             dexed_import_file_count_ > 0 ? "Hold=Import" : "");
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
        case EnsembleParamPage::Perc: page_name = "Perc"; break;
        case EnsembleParamPage::Click: page_name = "Click"; break;
        case EnsembleParamPage::Env: page_name = "Env"; break;
        case EnsembleParamPage::Ensemble: page_name = "Chorus"; break;
        case EnsembleParamPage::Lfo: page_name = "LFO"; break;
        case EnsembleParamPage::LfoDepth: page_name = "LFODep"; break;
        case EnsembleParamPage::Filter: page_name = "Filter"; break;
        case EnsembleParamPage::Mix: page_name = "Mix"; break;
        case EnsembleParamPage::FX: page_name = "FX"; break;
        case EnsembleParamPage::MIDI: page_name = "MIDI"; break;
        case EnsembleParamPage::Preset: page_name = "Preset"; break;
        case EnsembleParamPage::User: page_name = "User"; break;
        default: break;
    }
    char title[24];
    snprintf(title, sizeof(title), "%s:%s", ensemble_active_ == 1 ? "Organ" : "Ensmbl", page_name);
    DrawPageTitle(title, EnsembleUnsaved());
    DrawBeatIndicator(disp_->Width() - 41, 0, 3);
    disp_->DrawLine(0, 9, disp_->Width() - 1, 9, true);

    switch(ensemble_param_page_)
    {
        case EnsembleParamPage::Registration:
        {
            // Organ-stop names, same order as EnsembleSynth::
            // SetDrawbarLevel()'s own idx (0=Saw8' .. 6=Saw1'), and the
            // Organ's nine in Hammond order (see EnsembleSynth::SetOrgan()).
            static const char* const kDrawbarNames[EnsembleSynth::kNumDrawbars]
                = {"Saw 8'", "Sq 8'", "Saw 4'", "Sq 4'", "Saw 2'", "Sq 2'", "Saw 1'"};
            static const char* const kOrganDrawbarNames[EnsembleSynth::kNumOrganDrawbars]
                = {"16'", "5 1/3'", "8'", "4'", "2 2/3'", "2'", "1 3/5'", "1 1/3'", "1'"};
            const bool organ = ensemble_->IsOrgan();
            const int  nbars = ensemble_->NumDrawbars();
            int        idx   = ensemble_drawbar_index_;
            if(idx < 0 || idx >= nbars)
                idx = 0;
            TomThumbDrawText(disp_, 0, 16, organ ? kOrganDrawbarNames[idx] : kDrawbarNames[idx], true);

            // One vertical fader per drawbar -- K1 (above) picks which
            // one is selected, K2 edits that one's level; drawn instead
            // of a scope so the whole registration is visible at a
            // glance, same "see every channel, not just the one you're
            // touching" reasoning Screen::Mixer's own bars already use.
            const int kBarTop = 19, kBarBottom = kFooterDividerY - 2;
            const int kBarGap = organ ? 2 : 3;
            const int kBarW   = (disp_->Width() - kBarGap * (nbars - 1)) / nbars;
            const int kRowW   = kBarW * nbars + kBarGap * (nbars - 1);
            const int kX0     = (disp_->Width() - kRowW) / 2;
            for(int i = 0; i < nbars; i++)
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
        case EnsembleParamPage::Perc:
        {
            // The percussion's decay as a curve: a 1 s window, so Fast
            // drops away quickly and Slow stretches across.
            const bool on = ensemble_->GetPercOn();
            char       line[24];
            snprintf(line, sizeof(line), "%s  %s harmonic", on ? "ON" : "OFF",
                      ensemble_->GetPercThird() ? "3RD" : "2ND");
            TomThumbDrawText(disp_, 0, 16, line, true);
            const int   x0 = 4, x1 = disp_->Width() - 5, y0 = 19, y1 = kFooterDividerY - 3;
            const float tau = ensemble_->GetPercDecaySeconds() / 6.9078f;
            const float amp = 0.3f + 0.7f * ensemble_->GetPercLevel01();
            int         py  = -1;
            for(int x = x0; x <= x1; x++)
            {
                float t = 1.5f * (float)(x - x0) / (float)(x1 - x0);
                int   y = y1 - (int)((float)(y1 - y0) * amp * expf(-t / tau) + 0.5f);
                if(on || (x & 1)) // dotted while Off
                    disp_->DrawLine(x, py < 0 ? y : py, x, y, true);
                py = y;
            }
            disp_->DrawLine(x0, y1 + 1, x1, y1 + 1, true);
            char d_val[8], v_val[8];
            // Whole numbers only: this printf has no float support
            // (a %f draws junk), so hundredths are split out by hand.
            const int cs = (int)(ensemble_->GetPercDecaySeconds() * 100.f + 0.5f);
            snprintf(d_val, sizeof(d_val), "%d.%02ds", cs / 100, cs % 100);
            snprintf(v_val, sizeof(v_val), "%d%%",
                      (int)(ensemble_->GetPercLevel01() * 100.f + 0.5f));
            DrawControlRow(kFooterRow1Y, false, kFooterDividerY, "Decay", d_val, v_val, "Volume");
            DrawControlRow(kFooterRow2Y, true, kFooterInterRowDividerY,
                             on ? "Perc:On* Off" : "Perc:On Off*", "", "",
                             ensemble_->GetPercThird() ? "2nd 3rd*" : "2nd* 3rd");
            break;
        }
        case EnsembleParamPage::Click:
        {
            DrawOscilloscope(12, kFooterDividerY - 2, ensemble_scope_buf_, ensemble_scope_capacity_);
            char c_val[8];
            if(ensemble_->GetClick01() <= 0.f)
                snprintf(c_val, sizeof(c_val), "Off");
            else
                snprintf(c_val, sizeof(c_val), "%d%%", (int)(ensemble_->GetClick01() * 100.f + 0.5f));
            DrawControlRow(kFooterRow1Y, false, kFooterDividerY, "Click", c_val, "", "");
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
        case EnsembleParamPage::Lfo: DrawLfoPage(ensemble_->Lfo()); break;
        case EnsembleParamPage::LfoDepth: DrawLfoDepthPage(ensemble_->Lfo(), ensemble_lfo_dest_); break;
        case EnsembleParamPage::Filter:
        {
            DrawFilterCurve(ensemble_->GetFilterMode(), ensemble_->GetFilterCutoff01(),
                            ensemble_->GetFilterResonance01());
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
            // The Ensemble's or the Organ's mixer channel.
            DrawMixPageScope(12, ensemble_scope_buf_, ensemble_scope_capacity_,
                             ensemble_active_ == 1 ? kMixOrg : kMixEns);
            // K1 Level, K2 Gain into the limiter; Pan is on the Mixer.
            char out_val[8], gain_val[12];
            snprintf(out_val, sizeof(out_val), "%d%%",
                     (int)(ensemble_->GetOutputLevel01() * 100.f + 0.5f));
            FormatGainDb(ensemble_->GetGain01(), gain_val, sizeof(gain_val));
            DrawControlRow(kFooterRow1Y, false, kFooterDividerY, "Level", out_val, gain_val, "Gain");
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
                DrawControlRow(kFooterRow1Y, false, kFooterDividerY, "Send", send_val, size_val,
                                 "Size");
            }
            else
            {
                char send_val[8], time_val[12];
                snprintf(send_val, sizeof(send_val), "%d%%",
                          (int)(ensemble_->GetDelaySend01() * 100.f + 0.5f));
                FormatDelayTime(delay_time01_, time_val, sizeof(time_val));
                DrawControlRow(kFooterRow1Y, false, kFooterDividerY, "Send", send_val, time_val,
                                 "Time");
            }
            DrawFxSends(ensemble_->GetReverbSend01(), ensemble_->GetDelaySend01(), ensemble_fx_target_delay_ ? 1 : 0);
            DrawControlRow(kFooterRow2Y, true, kFooterInterRowDividerY,
                             ensemble_fx_target_delay_ ? "Reverb" : "Reverb*", "", "",
                             ensemble_fx_target_delay_ ? "Delay*" : "Delay");
            break;
        }
        case EnsembleParamPage::MIDI:
        {
            char chan_val[8];
            const int ch = *EnsembleActiveChannelPtr();
            if(ch < 0)
                snprintf(chan_val, sizeof(chan_val), "Omni");
            else
                snprintf(chan_val, sizeof(chan_val), "%d", ch + 1);
            char wheel_line[24];
            snprintf(wheel_line, sizeof(wheel_line), "Wheel: %s", ensemble_->GetModWheelTargetName());
            TomThumbDrawText(disp_, 0, 16, wheel_line, true);
            DrawOscilloscope(18, kFooterDividerY - 2, ensemble_scope_buf_, ensemble_scope_capacity_);
            char voices_val[8];
            snprintf(voices_val, sizeof(voices_val), "%d", ensemble_->GetMaxVoices());
            const bool organ = ensemble_->IsOrgan();
            if(ensemble_midi_vel_ && !organ)
            {
                char vel_val[8];
                snprintf(vel_val, sizeof(vel_val), "%d%%",
                         (int)(ensemble_->GetVelSens01() * 100.f + 0.5f));
                DrawControlRow(kFooterRow1Y, false, kFooterDividerY, "Velocity",
                               ensemble_->GetVelSens01() > 0.f ? vel_val : "Off", "", "");
            }
            else
                DrawControlRow(kFooterRow1Y, false, kFooterDividerY, "MIDI Ch", chan_val,
                               voices_val, "Voices");
            DrawControlRow(kFooterRow2Y, true, kFooterInterRowDividerY, "Cycle wheel tgt", "", "", "");
            if(!organ)
                DrawStarToggle(false, "Ch/Vel", ensemble_midi_vel_);
            break;
        }
        case EnsembleParamPage::Preset:
        case EnsembleParamPage::User:
        {
            // Only the User page needs the card -- the Preset page's
            // factory sounds are built into the firmware.
            if(ensemble_param_page_ == EnsembleParamPage::User && !PerformanceStore::IsCardPresent())
            {
                disp_->SetCursor(0, 20);
                WriteUpper("No card");
                DrawControlRow(kFooterRow1Y, false, kFooterDividerY, "", "", "", "");
                DrawControlRow(kFooterRow2Y, true, kFooterInterRowDividerY, "Retry", "", "", "");
                break;
            }
            if(ensemble_preset_slots_dirty_)
            {
                RefreshEnsemblePresetSlots();
                if(ensemble_user_cursor_ > ensemble_preset_user_slot_count_)
                    ensemble_user_cursor_ = ensemble_preset_user_slot_count_;
            }

            bool is_user       = ensemble_param_page_ == EnsembleParamPage::User;
            int  loaded        = ensemble_loaded_preset_slot_;
            bool choosing_save = save_load_mode_ == SaveLoadMode::ChoosingSave;
            bool browsing_load = save_load_mode_ == SaveLoadMode::BrowsingLoad;

            // Saving keeps its hold-to-confirm bar; loading is a plain tap.
            if(pod_->button2.Pressed() && choosing_save)
            {
                float held = pod_->button2.TimeHeldMs();
                int   w    = (int)(Clampf(held / 800.f, 0.f, 1.f) * (disp_->Width() - 2));
                disp_->SetCursor(0, 20);
                WriteUpper("Hold: Save...");
                disp_->DrawRect(0, 30, disp_->Width() - 1, 34, true, false);
                if(w > 0)
                    disp_->DrawRect(1, 31, w, 33, true, true);
                DrawControlRow(kFooterRow1Y, false, kFooterDividerY, "", "", "", "");
                DrawControlRow(kFooterRow2Y, true, kFooterInterRowDividerY, "", "", "", "");
                break;
            }

            TomThumbDrawText(disp_, 0, 16, is_user ? "User Presets" : "Factory Presets", true);

            if(choosing_save)
            {
                bool can_overwrite = EnsembleIsUserSlot(loaded);
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
                // "> " marks the sound that's currently loaded. "ENSM"
                // matches Global:SdMgmt's own short code for user saves.
                char line[24];
                if(!is_user)
                {
                    int slot = kEnsembleFactorySlotBase + ensemble_preset_cursor_;
                    snprintf(line, sizeof(line), "%s%s", slot == loaded ? "> " : "",
                              ensemble_->GetFactoryPresetName(ensemble_preset_cursor_));
                }
                else if(ensemble_user_cursor_ == 0)
                    snprintf(line, sizeof(line), "New (blank)");
                else
                {
                    int slot = ensemble_preset_user_slots_[ensemble_user_cursor_ - 1];
                    snprintf(line, sizeof(line), "%s%s%d", slot == loaded ? "> " : "",
                              EnsembleSlotPrefix(), slot);
                }
                disp_->SetCursor(0, 24);
                WriteUpper(line);
            }
            else
            {
                char line1[24];
                if(loaded <= 0)
                    snprintf(line1, sizeof(line1), "Now: (new)");
                else if(EnsembleIsFactorySlot(loaded))
                    snprintf(line1, sizeof(line1), "Now: %s",
                              ensemble_->GetFactoryPresetName(loaded
                                                                  - kEnsembleFactorySlotBase));
                else
                    snprintf(line1, sizeof(line1), "Now: %s%d", EnsembleSlotPrefix(), loaded);
                disp_->SetCursor(0, 24);
                WriteUpper(line1);
                if(ensemble_preset_status_[0] != '\0')
                {
                    disp_->SetCursor(0, 36);
                    WriteUpper(ensemble_preset_status_);
                }
            }

            const char* b1_label = save_load_mode_ != SaveLoadMode::Idle ? "Back"
                                   : is_user                             ? "Save"
                                                                         : "";
            const char* b2_label = choosing_save ? "Hold=Save" : "Load";
            DrawControlRow(kFooterRow1Y, false, kFooterDividerY, choosing_save ? "Select" : "",
                             "", "", browsing_load ? "Scroll" : "");
            DrawControlRow(kFooterRow2Y, true, kFooterInterRowDividerY, b1_label, "", "",
                             b2_label);
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
    s.Add(sample_synth_->GetGain01());
    s.Add(sample_synth_->GetVelSens01());
    s.Add(sample_synth_mode_ == SampleSynthMode::Drum ? sample_drum_midi_channel_
                                                      : sample_synth_midi_channel_);
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
            DrawFilterCurve(sample_synth_->GetFilterMode(), sample_synth_->GetFilterCutoff01(),
                            sample_synth_->GetFilterResonance01());

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
            // Same layout as EnsembleParamPage::Mix.
            DrawMixPageScope(12, sample_synth_scope_buf_, sample_synth_scope_capacity_,
                               sample_synth_mode_ == SampleSynthMode::Drum ? kMixSmd : kMixSmv);
            if(sample_synth_mix_target_gain_)
            {
                char gain_val[12];
                FormatGainDb(sample_synth_->GetGain01(), gain_val, sizeof(gain_val));
                DrawControlRow(kFooterRow1Y, false, kFooterDividerY, "Gain", gain_val, "", "");
            }
            else
            {
                char out_val[8], pan_val[8];
                snprintf(out_val, sizeof(out_val), "%d%%",
                          (int)(sample_synth_->GetOutputLevel01() * 100.f + 0.5f));
                snprintf(pan_val, sizeof(pan_val), "%d%%",
                          (int)(sample_synth_->GetPan01() * 100.f + 0.5f));
                DrawControlRow(kFooterRow1Y, false, kFooterDividerY, "Level", out_val, pan_val,
                                 "Pan");
            }
            DrawControlRow(kFooterRow2Y, true, kFooterInterRowDividerY,
                             sample_synth_mix_target_gain_ ? "Level" : "Level*", "", "",
                             sample_synth_mix_target_gain_ ? "Gain*" : "Gain");
            break;
        }
        case SampleSynthParamPage::Setup:
        {
            // Mono/Choke moved here from Mix -- see
            // SampleSynthPresetData::mono_choke's own comment. Same
            // status-line position every other page's own first
            // TomThumb line uses right under the header divider, a
            // pixel clear of it (TomThumbDrawText's y is the text's
            // BASELINE: its tallest glyphs reach 5 rows up, so 16 leaves
            // row 10 blank under the divider at 9) -- naming just
            // which state is currently selected, same "the button
            // label names the option, the status line names what's
            // active" split as Env's own AD/AD*-SR/SR* footer.
            TomThumbDrawText(disp_, 0, 17, sample_synth_->GetMonoChoke() ? "Mono" : "Poly",
                             true); // clear of the title's line, left of the grid
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
                // Centred in the whole body, header divider (y=9) to footer
                // divider: the status line sits left of the grid, clear of it.
                constexpr int kBodyTop = 10;
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
                DrawControlRow(kFooterRow1Y, false, kFooterDividerY, "Send", send_val, size_val,
                                 "Size");
            }
            else
            {
                char send_val[8], time_val[12];
                snprintf(send_val, sizeof(send_val), "%d%%",
                          (int)(sample_synth_->GetDelaySend01() * 100.f + 0.5f));
                FormatDelayTime(delay_time01_, time_val, sizeof(time_val));
                DrawControlRow(kFooterRow1Y, false, kFooterDividerY, "Send", send_val, time_val,
                                 "Time");
            }
            DrawFxSends(sample_synth_->GetReverbSend01(), sample_synth_->GetDelaySend01(),
                        sample_synth_fx_target_delay_ ? 1 : 0);
            DrawControlRow(kFooterRow2Y, true, kFooterInterRowDividerY,
                             sample_synth_fx_target_delay_ ? "Reverb" : "Reverb*", "", "",
                             sample_synth_fx_target_delay_ ? "Delay*" : "Delay");
            break;
        }
        case SampleSynthParamPage::MIDI:
        {
            char chan_val[8];
            const int smp_ch = *SampleMidiChannel();
            if(smp_ch < 0)
                snprintf(chan_val, sizeof(chan_val), "Omni");
            else
                snprintf(chan_val, sizeof(chan_val), "%d", smp_ch + 1);
            // No body text of its own needed (title already says "MIDI")
            // -- same idiom as DexedParamPage::MIDI's own draw block.
            DrawOscilloscope(12, kFooterDividerY - 2, sample_synth_scope_buf_,
                               sample_synth_scope_capacity_);
            char sens_val[8];
            snprintf(sens_val, sizeof(sens_val), "%d%%",
                      (int)(sample_synth_->GetVelSens01() * 100.f + 0.5f));
            DrawControlRow(kFooterRow1Y, false, kFooterDividerY, "MIDI Ch", chan_val, sens_val,
                             "VelSens");
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
                TomThumbDrawText(disp_, 0, 16, hold_msg, true);
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
            TomThumbDrawText(disp_, 0, 16, src_line, true);

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
                        TomThumbDrawText(disp_, 0, 23 + row * 6, line, true);
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
                TomThumbDrawText(disp_, 0, 16, "Hold: Save...", true);
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
            TomThumbDrawText(disp_, 0, 16, line, true);

            // Reuses DrawGranularWaveform()'s own two-marker drawing as-
            // is -- a generic Ui:: method despite the name, not Grains-
            // coupled (see sample_synth_capture_peaks_'s own comment).
            DrawGranularWaveform(sample_synth_capture_peaks_, sample_synth_trim_start01_,
                                  &sample_synth_trim_end01_, 1, sample_synth_capture_full_len_ > 0);

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
            TomThumbDrawText(disp_, 0, 16, mono_line, true);
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
            if(pod_->button2.Pressed() && SampleSynthImportReady())
            {
                float held = pod_->button2.TimeHeldMs();
                int   w    = (int)(Clampf(held / 800.f, 0.f, 1.f) * (disp_->Width() - 2));
                TomThumbDrawText(disp_, 0, 16, "Hold: Import...", true);
                disp_->DrawRect(0, 20, disp_->Width() - 1, 24, true, false);
                if(w > 0)
                    disp_->DrawRect(1, 21, w, 23, true, true);
                DrawControlRow(kFooterRow1Y, false, kFooterDividerY, "", "", "", "");
                DrawControlRow(kFooterRow2Y, true, kFooterInterRowDividerY, "", "", "", "");
                break;
            }

            if(!PerformanceStore::IsCardPresent())
            {
                disp_->SetCursor(0, 20);
                WriteUpper("No card");
                DrawControlRow(kFooterRow1Y, false, kFooterDividerY, "", "", "", "");
                DrawControlRow(kFooterRow2Y, true, kFooterInterRowDividerY, "Retry", "", "", "");
                break;
            }
            if(sample_synth_import_dirty_)
                RefreshSampleSynthImport();
            bool voice = sample_synth_mode_ == SampleSynthMode::Voice;
            TomThumbDrawText(disp_, 0, 16, voice ? "Import Sounds" : "Import Drums", true);
            char line[40];

            // SMPDR, first step: Kits or Breaks.
            if(!SampleSynthImportReady())
            {
                for(int row = 0; row < 2; row++)
                {
                    bool brk = row == 1;
                    int  n   = brk ? sample_synth_import_breaks_ : sample_synth_import_kits_;
                    snprintf(line, sizeof(line), "%c%-7s%d waiting",
                              brk == sample_synth_import_drum_breaks_ ? '>' : ' ',
                              brk ? "Breaks" : "Kits", n);
                    TomThumbDrawText(disp_, 0, 23 + row * 6, line, true);
                }
                if(sample_synth_import_loose_ > 0)
                {
                    snprintf(line, sizeof(line), "%d old .aif: move to", sample_synth_import_loose_);
                    TomThumbDrawText(disp_, 0, 37, line, true);
                    TomThumbDrawText(disp_, 0, 43, "DRUM PACK IMPORT/KITS|BREAKS", true);
                }
                DrawControlRow(kFooterRow1Y, false, kFooterDividerY, "Type", "", "", "");
                DrawControlRow(kFooterRow2Y, true, kFooterInterRowDividerY, "", "", "", "Open");
                break;
            }

            bool breaks  = !voice && sample_synth_import_drum_breaks_;
            int  waiting = voice    ? sample_synth_import_sounds_
                           : breaks ? sample_synth_import_breaks_
                                    : sample_synth_import_kits_;
            const char* noun = voice ? "sound" : breaks ? "break" : "kit";
            if(waiting == 0)
                snprintf(line, sizeof(line), "No %ss waiting", noun);
            else
                snprintf(line, sizeof(line), "%d %s%s waiting", waiting, noun,
                          waiting == 1 ? "" : "s");
            disp_->SetCursor(0, 22);
            WriteUpper(line);

            // Where it will go.
            if(waiting > 0)
            {
                const ImportDest& d = sample_synth_import_dests_[sample_synth_import_dest_cursor_];
                char              dline[40];
                if(d.number == 0)
                    snprintf(dline, sizeof(dline), "Dest: New %ss bank", noun);
                else
                {
                    SampleBankEntry e;
                    e.type   = SampleSynthImportType();
                    e.number = (uint8_t)d.number;
                    e.items  = (int8_t)(PerformanceStore::kSampleBankCapacity - d.free);
                    char label[32];
                    SampleBankEntryLabel(e, label, sizeof(label));
                    snprintf(dline, sizeof(dline), "Dest: %s", label);
                }
                TomThumbDrawText(disp_, 0, 37, dline, true);
            }
            if(sample_synth_pack_import_status_[0] != '\0')
                TomThumbDrawText(disp_, 0, 44, sample_synth_pack_import_status_, true);

            DrawControlRow(kFooterRow1Y, false, kFooterDividerY,
                             waiting > 0 && sample_synth_import_dest_count_ > 1 ? "Dest" : "", "",
                             "", "");
            DrawControlRow(kFooterRow2Y, true, kFooterInterRowDividerY, voice ? "" : "Back", "",
                             "", waiting > 0 ? "Hold=Import" : "");
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

            bool at_type_select = AtSampleSynthVoiceTypeSelect();
            if(!at_type_select
               && (sample_synth_banks_dirty_ || sample_synth_banks_drum_ != drum_face
                   || sample_synth_banks_voice_type_ != sample_synth_voice_type_))
            {
                sample_synth_bank_count_ = ListFaceSampleBanks(
                    drum_face, sample_synth_banks_, kMaxListedSampleBanks, sample_synth_voice_type_);
                sample_synth_banks_drum_       = drum_face;
                sample_synth_banks_voice_type_ = sample_synth_voice_type_;
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
            static const char* const kVoiceTypeNames[kNumVoiceBankTypes]
                = {"Sounds", "Captured", "Multi"};
            static const char* const kDrumTypeNames[kNumDrumBankTypes] = {"Kits", "Breaks"};
            const char* const*       type_names = drum_face ? kDrumTypeNames : kVoiceTypeNames;
            char mode_line[24];
            if(browsing_load && at_type_select)
                snprintf(mode_line, sizeof(mode_line), "%s",
                          drum_face ? "Kits/Breaks" : "Sounds/Captured/Multi");
            else if(!at_category_select)
                SampleBankEntryLabel(sample_synth_banks_[sample_synth_sound_category_], mode_line,
                                     sizeof(mode_line));
            else
                snprintf(mode_line, sizeof(mode_line), "%s",
                          type_names[sample_synth_voice_type_ < 0 ? 0 : sample_synth_voice_type_]);
            TomThumbDrawText(disp_, 0, 16, mode_line, true);

            if(browsing_load)
            {
                char line2[24];
                if(at_type_select)
                    snprintf(line2, sizeof(line2), "%s",
                              type_names[sample_synth_voice_type_cursor_]);
                else if(at_category_select && sample_synth_bank_count_ > 0)
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
                                 : (at_type_select ? "Open"
                                    : at_category_select
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
                RefreshSampleSynthVoiceUserSlots(); // also reads each save's name
            else if(!is_voice && sample_synth_drum_user_dirty_)
                RefreshSampleSynthDrumUserSlots(); // also sorts saves into Kits/Breaks

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

            bool drum_folders = !is_voice && browsing_load && sample_synth_drum_user_folder_ < 0;
            bool drum_in_folder = !is_voice && browsing_load && sample_synth_drum_user_folder_ >= 0;
            static const char* const kDrumFolders[2] = {"Kits", "Breaks"};
            TomThumbDrawText(disp_, 0, 16,
                              is_voice         ? "Sound Presets"
                              : drum_folders   ? "Kits+Breaks"
                              : drum_in_folder ? kDrumFolders[sample_synth_drum_user_folder_]
                                               : "Kit Presets",
                              true);

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
                if(drum_folders)
                    snprintf(line2, sizeof(line2), "%s (%d)",
                              kDrumFolders[sample_synth_drum_user_folder_cursor_],
                              sample_synth_drum_user_folder_count_[sample_synth_drum_user_folder_cursor_]);
                else if(drum_in_folder
                        && sample_synth_drum_user_folder_count_[sample_synth_drum_user_folder_] == 0)
                    snprintf(line2, sizeof(line2), "(empty)");
                else if(count > 0)
                {
                    char nm[kUserSaveNameLen];
                    SampleSynthUserSaveName(is_voice, cursor, nm, sizeof(nm));
                    snprintf(line2, sizeof(line2), "%s%s", numbers[cursor] == loaded_slot ? "> " : "",
                              nm);
                }
                else
                    snprintf(line2, sizeof(line2), "(none saved yet)");
                disp_->SetCursor(0, 24);
                WriteUpper(line2);
            }
            else
            {
                char line1[24];
                int loaded_i = -1;
                for(int i = 0; i < count; i++)
                    if(numbers[i] == loaded_slot)
                        loaded_i = i;
                if(loaded_slot <= 0)
                    snprintf(line1, sizeof(line1), "Now: (none)");
                else if(loaded_i >= 0)
                {
                    char nm[kUserSaveNameLen];
                    SampleSynthUserSaveName(is_voice, loaded_i, nm, sizeof(nm));
                    snprintf(line1, sizeof(line1), "Now: %s", nm);
                }
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

            DrawControlRow(kFooterRow1Y, false, kFooterDividerY, drum_folders ? "Scroll" : "", "", "",
                             browsing_load && !drum_folders ? "Scroll" : "");
            DrawControlRow(kFooterRow2Y, true, kFooterInterRowDividerY,
                             save_load_mode_ == SaveLoadMode::Idle ? "Save" : "Back", "", "",
                             save_load_mode_ == SaveLoadMode::Idle
                                 ? "Load"
                                 : (drum_folders
                                        ? "Open"
                                        : (browsing_load ? (count > 0 ? "Load" : "")
                                                         : (choosing_save ? "Hold=Save" : ""))));
            break;
        }
        default: break;
    }
}

void Ui::DexedEditBytesFor(KnobContext ctx, DexedEditByte* k1, DexedEditByte* k2) const
{
    // Operator bytes are relative to the selected operator's own 21-byte
    // block; voice bytes are absolute. Byte map: see dexed_synth.cpp.
    int b = dexed_op_index_ * 21;
    *k1   = {-1, 0};
    *k2   = {-1, 0};
    switch(ctx)
    {
        case KnobContext::DexedOpAlgo: *k1 = {134, 31}; break; // ALGORITHM 1-32
        case KnobContext::DexedOpFrequency: *k1 = {b + 18, 31}; *k2 = {b + 19, 99}; break;
        case KnobContext::DexedOpDetune: *k1 = {b + 20, 14}; break;
        case KnobContext::DexedOpOutputLevel: *k1 = {b + 16, 99}; break;
        case KnobContext::DexedOpEgRate12: *k1 = {b + 0, 99}; *k2 = {b + 1, 99}; break;
        case KnobContext::DexedOpEgRate34: *k1 = {b + 2, 99}; *k2 = {b + 3, 99}; break;
        case KnobContext::DexedOpEgLevel12: *k1 = {b + 4, 99}; *k2 = {b + 5, 99}; break;
        case KnobContext::DexedOpEgLevel34: *k1 = {b + 6, 99}; *k2 = {b + 7, 99}; break;
        case KnobContext::DexedOpLevelScalingCurve: *k1 = {b + 11, 3}; *k2 = {b + 12, 3}; break;
        case KnobContext::DexedOpBreakPoint: *k1 = {b + 8, 99}; *k2 = {b + 12, 3}; break;
        case KnobContext::DexedOpLevelScalingDepth: *k1 = {b + 9, 99}; *k2 = {b + 10, 99}; break;
        case KnobContext::DexedOpRateScaling: *k1 = {b + 13, 7}; break;
        case KnobContext::DexedOpSensitivity: *k1 = {b + 15, 7}; *k2 = {b + 14, 3}; break;
        case KnobContext::DexedVoicePitchEgRate12: *k1 = {126, 99}; *k2 = {127, 99}; break;
        case KnobContext::DexedVoicePitchEgRate34: *k1 = {128, 99}; *k2 = {129, 99}; break;
        case KnobContext::DexedVoicePitchEgLevel12: *k1 = {130, 99}; *k2 = {131, 99}; break;
        case KnobContext::DexedVoicePitchEgLevel34: *k1 = {132, 99}; *k2 = {133, 99}; break;
        case KnobContext::DexedVoiceLfo: *k1 = {137, 99}; *k2 = {138, 99}; break;
        case KnobContext::DexedVoiceLfoDepth: *k1 = {139, 99}; *k2 = {140, 99}; break;
        case KnobContext::DexedVoiceModSens: *k1 = {143, 7}; break;
        case KnobContext::DexedVoiceTranspose: *k1 = {144, 48}; break;
        default: break;
    }
}

void Ui::OnDexedOperatorButton2()
{
    if(!dexed_)
        return;
    int b = dexed_op_index_ * 21;
    switch(dexed_op_page_)
    {
        case DexedOpParamPage::Frequency: // OSC MODE RATIO <-> FIXED
            dexed_->SetPatchByte(b + 17, dexed_->GetPatchByte(b + 17) ? 0 : 1);
            break;
        case DexedOpParamPage::OutputLevel: // OP ON/OFF
            dexed_->SetOpSwitch(dexed_->GetOpSwitch() ^ (uint8_t)(1 << dexed_op_index_));
            break;
        // Pair toggles -- these change CurrentKnobContext(), so pickup
        // re-arms on its own (see ApplyKnobs()).
        case DexedOpParamPage::EgRate: dexed_op_egrate_target34_ = !dexed_op_egrate_target34_; break;
        case DexedOpParamPage::EgLevel: dexed_op_eglevel_target34_ = !dexed_op_eglevel_target34_; break;
        case DexedOpParamPage::LevelScalingCurve: dexed_op_bp_target_ = !dexed_op_bp_target_; break;
        case DexedOpParamPage::PitchEgRate:
            dexed_pitch_egrate_target34_ = !dexed_pitch_egrate_target34_;
            break;
        case DexedOpParamPage::PitchEgLevel:
            dexed_pitch_eglevel_target34_ = !dexed_pitch_eglevel_target34_;
            break;
        case DexedOpParamPage::Lfo: // LFO WAVE, 6 shapes
            dexed_->SetPatchByte(142, (uint8_t)((dexed_->GetPatchByte(142) + 1) % 6));
            break;
        case DexedOpParamPage::LfoDepth: // LFO KEY SYNC
            dexed_->SetPatchByte(141, dexed_->GetPatchByte(141) ? 0 : 1);
            break;
        case DexedOpParamPage::ModSens: // OSC KEY SYNC
            dexed_->SetPatchByte(136, dexed_->GetPatchByte(136) ? 0 : 1);
            break;
        default: break;
    }
}

namespace
{
// Yamaha note naming, middle C (MIDI 60) = C3 -- the DX7's own
// BREAK POINT/TRANSPOSE display.
void Dx7NoteName(int midi, char* out, size_t out_size)
{
    static const char* const kNames[12]
        = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
    int n = midi < 0 ? 0 : midi;
    snprintf(out, out_size, "%s%d", kNames[n % 12], n / 12 - 2);
}

const char* const kDx7CurveNames[4] = {"-LIN", "-EXP", "+EXP", "+LIN"};
const char* const kDx7LfoWaveNames[6]
    = {"TRIANGL", "SAW DWN", "SAW UP", "SQUARE", "SINE", "S/HOLD"};
} // namespace

void Ui::DrawDexedOperatorScreen()
{
    if(!dexed_)
        return;

    // Real HW op number (6-index), same convention the Algo diagram
    // already uses -- confirmed from the real DX7 SysEx spec that array
    // index 0 is HW OP6, index 5 is OP1.
    int  hw_op  = 6 - dexed_op_index_;
    int  base   = dexed_op_index_ * 21;
    bool voice  = IsDexedVoicePage(dexed_op_page_);
    bool op_on  = (dexed_->GetOpSwitch() >> dexed_op_index_) & 1;

    char title[16];
    if(voice)
        snprintf(title, sizeof(title), "VOICE");
    else
        snprintf(title, sizeof(title), op_on ? "OP%d" : "OP%d OFF", hw_op);
    DrawPageTitle(title, DexedUnsaved());
    DrawBeatIndicator(disp_->Width() - 41, 0, 3);
    disp_->DrawLine(0, 9, disp_->Width() - 1, 9, true);

    // The DX7's own name for what this page edits, under the title.
    const char* name = "";
    switch(dexed_op_page_)
    {
        case DexedOpParamPage::Algo: name = "ALGORITHM"; break;
        case DexedOpParamPage::Frequency: name = "OSC FREQUENCY"; break;
        case DexedOpParamPage::Detune: name = "OSC DETUNE"; break;
        case DexedOpParamPage::OutputLevel: name = "OUTPUT LEVEL"; break;
        case DexedOpParamPage::EgRate: name = "EG RATE"; break;
        case DexedOpParamPage::EgLevel: name = "EG LEVEL"; break;
        case DexedOpParamPage::LevelScalingCurve: name = "KEYBOARD LEVEL SCALING CURVE"; break;
        case DexedOpParamPage::LevelScalingDepth: name = "KEYBOARD LEVEL SCALING DEPTH"; break;
        case DexedOpParamPage::RateScaling: name = "KEYBOARD RATE SCALING"; break;
        case DexedOpParamPage::Sensitivity: name = "SENSITIVITY"; break;
        case DexedOpParamPage::PitchEgRate: name = "PITCH EG RATE"; break;
        case DexedOpParamPage::PitchEgLevel: name = "PITCH EG LEVEL"; break;
        case DexedOpParamPage::Lfo: name = "LFO"; break;
        case DexedOpParamPage::LfoDepth: name = "LFO DEPTH"; break;
        case DexedOpParamPage::ModSens: name = "P MOD SENS / OSC KEY SYNC"; break;
        case DexedOpParamPage::Transpose: name = "KEY TRANSPOSE"; break;
        default: break;
    }
    if(dexed_op_page_ != DexedOpParamPage::Algo)
        TomThumbDrawText(disp_, 0, 16, name, true);

    // Footer row 2: B1 = operator select on operator pages only.
    const char* b1 = voice ? "" : "OP";

    // One big centred 6x8 line in the body -- the page's main readout.
    auto big = [&](const char* text, int y) {
        int x = (disp_->Width() - (int)strlen(text) * 6) / 2;
        disp_->SetCursor(x < 0 ? 0 : x, y);
        WriteUpper(text);
    };
    auto pb = [&](int off) { return (int)dexed_->GetPatchByte(off); };

    char v1[12] = "", v2[12] = "", line[24];
    switch(dexed_op_page_)
    {
        case DexedOpParamPage::Algo:
            // K1 picks ALGORITHM 1-32 (the main Dexed Algo page only shows it).
            DrawAlgorithmDiagram(pb(134));
            snprintf(v1, sizeof(v1), "%d", pb(134) + 1);
            DrawControlRow(kFooterRow1Y, false, kFooterDividerY, "ALGORITHM", v1, "", "");
            DrawControlRow(kFooterRow2Y, true, kFooterInterRowDividerY, b1, "", "", "");
            break;
        case DexedOpParamPage::Frequency:
        {
            // Same frequency the engine plays (msfa osc_freq()): RATIO is
            // COARSE (0 = 0.50) x (1 + FINE/100); FIXED is 10^(COARSE&3)
            // Hz scaled up by FINE/100 of a decade.
            int  coarse = pb(base + 18), fine = pb(base + 19);
            bool fixed  = pb(base + 17) != 0;
            if(fixed)
            {
                float hz = powf(10.f, (float)(coarse & 3)) * powf(10.f, (float)fine / 100.f);
                // Whole numbers only -- no float printf (see the Organ's
                // Perc page).
                if(hz < 1000.f)
                {
                    const int t = (int)(hz * 10.f + 0.5f);
                    snprintf(line, sizeof(line), "FIXED %d.%dHz", t / 10, t % 10);
                }
                else
                    snprintf(line, sizeof(line), "FIXED %dHz", (int)(hz + 0.5f));
            }
            else
            {
                float ratio = (coarse == 0 ? 0.5f : (float)coarse) * (1.f + (float)fine / 100.f);
                const int r = (int)(ratio * 100.f + 0.5f);
                snprintf(line, sizeof(line), "RATIO %d.%02d", r / 100, r % 100);
            }
            big(line, 26);
            snprintf(v1, sizeof(v1), "%d", coarse);
            snprintf(v2, sizeof(v2), "%d", fine);
            DrawControlRow(kFooterRow1Y, false, kFooterDividerY, "F COARSE", v1, v2, "F FINE");
            DrawControlRow(kFooterRow2Y, true, kFooterInterRowDividerY, b1, "", "",
                           fixed ? "RATIO" : "FIXED");
            break;
        }
        case DexedOpParamPage::Detune:
        {
            int d = pb(base + 20) - 7;
            snprintf(v1, sizeof(v1), d > 0 ? "+%d" : "%d", d);
            big(v1, 26);
            DrawControlRow(kFooterRow1Y, false, kFooterDividerY, "DETUNE", v1, "", "");
            DrawControlRow(kFooterRow2Y, true, kFooterInterRowDividerY, b1, "", "", "");
            break;
        }
        case DexedOpParamPage::OutputLevel:
        {
            big(op_on ? "OPERATOR ON" : "OPERATOR OFF", 22);
            // Level bar, 0-99 across the body width.
            int lvl = pb(base + 16);
            disp_->DrawRect(1, 34, disp_->Width() - 2, 40, true, false);
            disp_->DrawRect(1, 34, 1 + lvl * (disp_->Width() - 3) / 99, 40, true, true);
            snprintf(v1, sizeof(v1), "%d", lvl);
            DrawControlRow(kFooterRow1Y, false, kFooterDividerY, "OUTPUT LEVEL", v1, "", "");
            DrawControlRow(kFooterRow2Y, true, kFooterInterRowDividerY, b1, "", "",
                           op_on ? "OFF" : "ON");
            break;
        }
        case DexedOpParamPage::EgRate:
        case DexedOpParamPage::EgLevel:
        case DexedOpParamPage::PitchEgRate:
        case DexedOpParamPage::PitchEgLevel:
        {
            bool pitch = dexed_op_page_ == DexedOpParamPage::PitchEgRate
                         || dexed_op_page_ == DexedOpParamPage::PitchEgLevel;
            bool rate_page = dexed_op_page_ == DexedOpParamPage::EgRate
                             || dexed_op_page_ == DexedOpParamPage::PitchEgRate;
            bool pair34 = dexed_op_page_ == DexedOpParamPage::EgRate    ? dexed_op_egrate_target34_
                          : dexed_op_page_ == DexedOpParamPage::EgLevel ? dexed_op_eglevel_target34_
                          : rate_page ? dexed_pitch_egrate_target34_
                                      : dexed_pitch_eglevel_target34_;
            int     rate_off  = pitch ? 126 : base;
            int     level_off = pitch ? 130 : base + 4;
            uint8_t rates[4], levels[4];
            for(int i = 0; i < 4; i++)
            {
                rates[i]  = (uint8_t)pb(rate_off + i);
                levels[i] = (uint8_t)pb(level_off + i);
            }
            uint8_t hl = pair34 ? 0x0c : 0x03;
            DrawDx7EnvelopeShape(18, 44, rates, levels, rate_page ? hl : 0, rate_page ? 0 : hl,
                                 pitch);
            int first = pair34 ? 2 : 0;
            int off   = rate_page ? rate_off : level_off;
            snprintf(v1, sizeof(v1), "%d", pb(off + first));
            snprintf(v2, sizeof(v2), "%d", pb(off + first + 1));
            char l1[4], l2[4];
            snprintf(l1, sizeof(l1), "%c%d", rate_page ? 'R' : 'L', first + 1);
            snprintf(l2, sizeof(l2), "%c%d", rate_page ? 'R' : 'L', first + 2);
            DrawControlRow(kFooterRow1Y, false, kFooterDividerY, l1, v1, v2, l2);
            DrawControlRow(kFooterRow2Y, true, kFooterInterRowDividerY, b1, "", "", "");
            {
                // B2: "1/2*  3/4" / "1/2  3/4*" -- every piece has a fixed
                // spot (the "*" slot is reserved after both), so only the
                // asterisk moves. Centred in the space right of the icon.
                const int star_w = TomThumbAdvanceWidth("*");
                const int w12    = TomThumbAdvanceWidth("1/2");
                const int w34    = TomThumbAdvanceWidth("3/4");
                const int gap    = 6;
                const int total  = w12 + star_w + gap + w34 + star_w;
                const int left   = disp_->Width() / 2 + 4;
                const int x      = left + (disp_->Width() - 1 - left - total) / 2;
                const int base_y = kFooterRow2Y + kTomThumbRowHeight - 1;
                const int x34    = x + w12 + star_w + gap;
                TomThumbDrawText(disp_, x, base_y, "1/2", true);
                TomThumbDrawText(disp_, x34, base_y, "3/4", true);
                TomThumbDrawText(disp_, pair34 ? x34 + w34 : x + w12, base_y, "*", true);
            }
            break;
        }
        case DexedOpParamPage::LevelScalingCurve:
        case DexedOpParamPage::LevelScalingDepth:
        {
            DrawLevelScalingGraph(19, 44, base);
            bool curve = dexed_op_page_ == DexedOpParamPage::LevelScalingCurve;
            if(curve)
            {
                if(dexed_op_bp_target_)
                    Dx7NoteName(pb(base + 8) + 21, v1, sizeof(v1));
                else
                    snprintf(v1, sizeof(v1), "%s", kDx7CurveNames[pb(base + 11) & 3]);
                snprintf(v2, sizeof(v2), "%s", kDx7CurveNames[pb(base + 12) & 3]);
                DrawControlRow(kFooterRow1Y, false, kFooterDividerY,
                               dexed_op_bp_target_ ? "BREAK POINT" : "L CURVE", v1, v2, "R CURVE");
                DrawControlRow(kFooterRow2Y, true, kFooterInterRowDividerY, b1, "", "",
                               dexed_op_bp_target_ ? "L CURVE" : "BREAK PT");
            }
            else
            {
                snprintf(v1, sizeof(v1), "%d", pb(base + 9));
                snprintf(v2, sizeof(v2), "%d", pb(base + 10));
                DrawControlRow(kFooterRow1Y, false, kFooterDividerY, "L DEPTH", v1, v2, "R DEPTH");
                DrawControlRow(kFooterRow2Y, true, kFooterInterRowDividerY, b1, "", "", "");
            }
            break;
        }
        case DexedOpParamPage::RateScaling:
        {
            snprintf(v1, sizeof(v1), "%d", pb(base + 13));
            big(v1, 24);
            TomThumbDrawText(disp_, 0, 42, "0 = SAME SPEED ON EVERY KEY", true);
            DrawControlRow(kFooterRow1Y, false, kFooterDividerY, "RATE SCALING", v1, "", "");
            DrawControlRow(kFooterRow2Y, true, kFooterInterRowDividerY, b1, "", "", "");
            break;
        }
        case DexedOpParamPage::Sensitivity:
        {
            snprintf(v1, sizeof(v1), "%d", pb(base + 15));
            snprintf(v2, sizeof(v2), "%d", pb(base + 14) & 3);
            TomThumbDrawText(disp_, 0, 26, "KEY VELOCITY  0-7", true);
            TomThumbDrawText(disp_, 0, 34, "A MOD SENS    0-3", true);
            TomThumbDrawText(disp_, 0, 42, "0 = NO RESPONSE", true);
            DrawControlRow(kFooterRow1Y, false, kFooterDividerY, "KEY VELOCITY", v1, v2,
                           "A MOD SENS");
            DrawControlRow(kFooterRow2Y, true, kFooterInterRowDividerY, b1, "", "", "");
            break;
        }
        case DexedOpParamPage::Lfo:
        {
            snprintf(line, sizeof(line), "%s", kDx7LfoWaveNames[pb(142) % 6]);
            big(line, 26);
            snprintf(v1, sizeof(v1), "%d", pb(137));
            snprintf(v2, sizeof(v2), "%d", pb(138));
            DrawControlRow(kFooterRow1Y, false, kFooterDividerY, "LFO SPEED", v1, v2, "LFO DELAY");
            DrawControlRow(kFooterRow2Y, true, kFooterInterRowDividerY, b1, "", "", "LFO WAVE");
            break;
        }
        case DexedOpParamPage::LfoDepth:
        {
            snprintf(line, sizeof(line), "KEY SYNC %s", pb(141) ? "ON" : "OFF");
            big(line, 26);
            snprintf(v1, sizeof(v1), "%d", pb(139));
            snprintf(v2, sizeof(v2), "%d", pb(140));
            DrawControlRow(kFooterRow1Y, false, kFooterDividerY, "PM DEPTH", v1, v2, "AM DEPTH");
            DrawControlRow(kFooterRow2Y, true, kFooterInterRowDividerY, b1, "", "",
                           "LFO KEY SYNC");
            break;
        }
        case DexedOpParamPage::ModSens:
        {
            snprintf(line, sizeof(line), "KEY SYNC %s", pb(136) ? "ON" : "OFF");
            big(line, 26);
            snprintf(v1, sizeof(v1), "%d", pb(143) & 7);
            DrawControlRow(kFooterRow1Y, false, kFooterDividerY, "P MOD SENS", v1, "", "");
            DrawControlRow(kFooterRow2Y, true, kFooterInterRowDividerY, b1, "", "",
                           "OSC KEY SYNC");
            break;
        }
        case DexedOpParamPage::Transpose:
        {
            int t = pb(144) > 48 ? 24 : pb(144);
            Dx7NoteName(t + 36, v1, sizeof(v1));
            snprintf(line, sizeof(line), "MIDDLE C = %s", v1);
            big(line, 26);
            DrawControlRow(kFooterRow1Y, false, kFooterDividerY, "TRANSPOSE", v1, "", "");
            DrawControlRow(kFooterRow2Y, true, kFooterInterRowDividerY, b1, "", "", "");
            break;
        }
        default: break;
    }
}

void Ui::DrawDx7EnvelopeShape(int top, int bottom, const uint8_t rates[4], const uint8_t levels[4],
                              uint8_t hl_rates, uint8_t hl_levels, bool center_line)
{
    // L1-L4 are numbered 7x7 squares (big enough for a Tom Thumb digit),
    // R1-R4 the numbered slopes joining them. A band at the top holds
    // the key-release "OFF" label.
    const int kSq          = 7;
    const int kHalf        = kSq / 2;
    const int label_bottom = top + 4;
    const int g_top        = top + 6 + kHalf; // highest square centre
    const int g_bottom     = bottom - kHalf;  // lowest square centre
    const int x0           = 1 + kHalf;
    const int x_end        = disp_->Width() - 2;
    // Key release sits at a fixed spot, so turning RATE 1-3 moves the
    // squares but RATE 4's slope always starts in the same place.
    const int x_off = 1 + (x_end - 1) * 70 / 100;
    // Every slope is at least this wide, so neighbouring squares never
    // overlap and there's room for the slope's own number.
    const float kMinSeg     = 13.f;
    const int   kMinSustain = 8;

    auto level_y = [&](int lvl) {
        lvl = lvl < 0 ? 0 : (lvl > 99 ? 99 : lvl);
        return g_bottom - lvl * (g_bottom - g_top) / 99;
    };
    // A RATE is a speed, not a time: the slope's width is the level
    // distance it travels times a factor that halves every 16 steps of
    // rate (higher = steeper).
    auto seg_w = [&](int from, int to, int rate) {
        int dist = from > to ? from - to : to - from;
        return (float)dist / 99.f * 28.f * powf(2.f, (50.f - (float)rate) / 16.f);
    };

    float w[4];
    int   prev = levels[3]; // every cycle starts from LEVEL 4
    for(int i = 0; i < 3; i++)
    {
        w[i] = seg_w(prev, levels[i], rates[i]);
        prev = levels[i];
    }
    w[3] = seg_w(levels[2], levels[3], rates[3]);
    // Slow rates would run past key release -- scale the slopes down
    // together (above the minimum) so their proportions stay true.
    float avail = (float)(x_off - x0 - kMinSustain) - 3.f * kMinSeg;
    float sum   = w[0] + w[1] + w[2];
    float scale = sum > avail ? avail / sum : 1.f;
    for(int i = 0; i < 4; i++)
        w[i] = kMinSeg + w[i] * scale;
    float r4_max = (float)(x_end - kHalf - x_off);
    if(w[3] > r4_max)
        w[3] = r4_max;

    if(center_line)
    {
        int cy = level_y(50);
        for(int x = 1; x <= x_end; x += 2)
            disp_->DrawPixel(x, cy, true);
    }
    // Key release marker -- dotted, labelled in the top band.
    for(int y = top + 6; y <= bottom; y += 2)
        disp_->DrawPixel(x_off, y, true);
    TomThumbDrawText(disp_, x_off - TomThumbInkWidth("OFF") / 2, label_bottom, "OFF", true);

    // Square centres: L4 (start), L1, L2, L3, then L4 again after R4.
    int sx[5], sy[5];
    sx[0]   = x0;
    sy[0]   = level_y(levels[3]);
    float x = (float)x0;
    for(int i = 0; i < 3; i++)
    {
        x += w[i];
        sx[i + 1] = (int)(x + 0.5f);
        sy[i + 1] = level_y(levels[i]);
    }
    int end_x = x_off + (int)(w[3] + 0.5f);
    int end_y = sy[0];

    // Slopes first, so the squares sit on top of their ends. Every line
    // stays 1px; what the knobs edit is marked on the numbers. Each
    // slope's number goes above it -- left of a rising slope, right of
    // a falling one, centred over a flat one -- so it never crosses the
    // line; below instead when there's no room above.
    auto slope = [&](int xa, int ya, int xb, int yb, int n, bool hl) {
        disp_->DrawLine(xa, ya, xb, yb, true);
        int  mx = (xa + xb) / 2, my = (ya + yb) / 2;
        char num[2] = {(char)('0' + n), 0};
        int  lx = yb < ya ? mx - 5 : (yb > ya ? mx + 3 : mx - 1);
        int  ly = my - 3;          // baseline: glyph spans my-7..my-3
        if(ly - 4 < top + 6)
            ly = my + 8;           // no room above -- glyph my+4..my+8
        // A rate the knobs are editing gets its number reversed out of a
        // small filled box -- the same marking as a filled level square.
        if(hl)
            disp_->DrawRect(lx - 1, ly - 5, lx + TomThumbInkWidth(num), ly + 1, true, true);
        TomThumbDrawText(disp_, lx, ly, num, !hl);
    };
    for(int i = 0; i < 3; i++)
        slope(sx[i], sy[i], sx[i + 1], sy[i + 1], i + 1, (hl_rates >> i) & 1);
    disp_->DrawLine(sx[3], sy[3], x_off, sy[3], true); // LEVEL 3 held until release
    slope(x_off, sy[3], end_x, end_y, 4, (hl_rates >> 3) & 1);

    // Squares: outlined with the number, or filled with the number
    // punched out for the levels the knobs are editing (same look as a
    // carrier box in the algorithm diagram).
    auto square = [&](int cx, int cy, int n, bool hl) {
        int xa = cx - kHalf, ya = cy - kHalf;
        disp_->DrawRect(xa, ya, xa + kSq - 1, ya + kSq - 1, false, true);
        disp_->DrawRect(xa, ya, xa + kSq - 1, ya + kSq - 1, true, hl);
        char num[2] = {(char)('0' + n), 0};
        int  ink_w  = TomThumbInkWidth(num);
        TomThumbDrawText(disp_, xa + 1 + (kSq - 2 - ink_w) / 2, ya + 6, num, !hl);
    };
    for(int i = 0; i < 3; i++)
        square(sx[i + 1], sy[i + 1], i + 1, (hl_levels >> i) & 1);
    square(sx[0], sy[0], 4, (hl_levels >> 3) & 1);
    square(end_x, end_y, 4, (hl_levels >> 3) & 1);
}

void Ui::DrawLevelScalingGraph(int top, int bottom, int base)
{
    // X = the DX7's keyboard range, A-1 (BREAK POINT 0) to C8 (99);
    // Y = the level change msfa's own ScaleLevel() adds at that key,
    // around a dotted "no change" line in the middle.
    if(!dexed_)
        return;
    const int x0  = 1;
    const int x1  = disp_->Width() - 2;
    const int mid = (top + bottom) / 2;
    const int bp  = dexed_->GetPatchByte(base + 8);
    for(int x = x0; x <= x1; x += 2)
        disp_->DrawPixel(x, mid, true);

    int prev_y = mid;
    for(int x = x0; x <= x1; x++)
    {
        int key   = 21 + (x - x0) * 99 / (x1 - x0);
        int delta = ScaleLevel(key, bp, dexed_->GetPatchByte(base + 9),
                               dexed_->GetPatchByte(base + 10), dexed_->GetPatchByte(base + 11),
                               dexed_->GetPatchByte(base + 12));
        // Output level is 0-127 internally; +-127 fills half the height.
        delta   = delta > 127 ? 127 : (delta < -127 ? -127 : delta);
        int y   = mid - delta * (mid - top) / 127;
        if(x == x0)
            prev_y = y;
        disp_->DrawLine(x - 1 < x0 ? x0 : x - 1, prev_y, x, y, true);
        prev_y = y;
    }

    // BREAK POINT marker: dotted line plus its note name.
    int bx = x0 + bp * (x1 - x0) / 99;
    for(int y = top; y <= bottom; y += 2)
        disp_->DrawPixel(bx, y, true);
    char nm[6];
    Dx7NoteName(bp + 21, nm, sizeof(nm));
    int w  = TomThumbInkWidth(nm);
    int tx = bx + 2 + w <= x1 ? bx + 2 : bx - 2 - w;
    TomThumbDrawText(disp_, tx, top + 5, nm, true);
}

void Ui::DrawMixerScreen()
{
    DrawBeatIndicator(disp_->Width() - 41, 0, 3);
    disp_->DrawLine(0, 9, disp_->Width() - 1, 9, true);

    if(mixer_position_ >= MixerSetCount()) // Scope stop
    {
        disp_->SetCursor(0, 0);
        WriteUpper("Mixer:Scope");
        TomThumbDrawText(disp_, 0, 16, "Final Mix", true);
        DrawOscilloscope(19, 44, master_scope_buf_, master_scope_capacity_);
        DrawControlRow(kFooterRow1Y, false, kFooterDividerY, "", "", "", "");
        DrawControlRow(kFooterRow2Y, true, kFooterInterRowDividerY, "", "", "", "");
        return;
    }

    // A real channel -- whichever group of 2 it belongs to (groups of 2,
    // not 4, so each channel gets a real 3rd bar for Peak alongside its
    // existing Vol/Rev pair -- see DrawMixerVBar()'s own doc comment for
    // why Peak lives on this page instead of a separate one).
    const int sel_ch    = MixerSetChannel(mixer_position_);
    bool      is_master = sel_ch == kMixMas;
    char      title[24];
    snprintf(title, sizeof(title), "Mixer:%s", MixerSetName(mixer_position_, true));
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
        const int pos = group_start + col;
        // 9 real channels (odd) since Ensemble was added -- the last
        // group has only one real channel (Master, alone); leave the
        // second column blank rather than reading/drawing channel 9
        // (out of range, MixerChannelName()/MixerGet*01() would all
        // silently fall through to their own "?"/0.f defaults, a real
        // but confusing-looking bug rather than a crash).
        if(pos >= MixerSetCount())
            break;
        const int ch       = MixerSetChannel(pos);
        bool      selected = pos == mixer_position_;
        int  col_x    = kColW * col;
        int  bars_w   = kBarW * 3 + kBarGap * 2;
        int  bars_x0  = col_x + (kColW - bars_w) / 2;

        if(selected)
            disp_->DrawRect(col_x + 1, 11, col_x + kColW - 2, 44, true, false);

        const char* name   = MixerSetName(pos, true);
        int         nw     = TomThumbAdvanceWidth(name);
        int         name_x = col_x + (kColW - nw) / 2;
        TomThumbDrawText(disp_, name_x, 17, name, true);

        bool  channel_is_master = ch == kMixMas;
        float vol01             = MixerGetVolume01(ch);
        float second01          = channel_is_master ? reverb_size01_ : MixerGetSend01(ch);

        DrawMixerVBar(bars_x0, kBarY0, kBarW, kBarH, vol01);
        DrawMixerVBar(bars_x0 + kBarW + kBarGap, kBarY0, kBarW, kBarH, second01);
        // Peak -- live signal level, not a fader setting (see
        // MixerGetLevel01()'s own comment); the clip-indicator latch
        // lives on this bar specifically, not Vol/Rev, so it's always
        // unambiguous which bar is "the meter."
        DrawMixerVBar(bars_x0 + (kBarW + kBarGap) * 2, kBarY0, kBarW, kBarH,
                       MeterBar01(MixerGetLevel01(ch)), MixerIsClipping(ch));

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
              (int)(MixerGetVolume01(sel_ch) * 100.f + 0.5f));
    if(is_master)
    {
        snprintf(second_val, sizeof(second_val), "%d%%", (int)(reverb_size01_ * 100.f + 0.5f));
        DrawControlRow(kFooterRow1Y, false, kFooterDividerY, "Vol", vol_val, second_val, "RevSz");
        DrawControlRow(kFooterRow2Y, true, kFooterInterRowDividerY, "", "", "", "");
    }
    else if(!mixer_target_reverb_)
    {
        if(MixerChannelHasPan(sel_ch))
        {
            char pan_val[8];
            snprintf(pan_val, sizeof(pan_val), "%d%%",
                      (int)(MixerGetPan01(sel_ch) * 100.f + 0.5f));
            DrawControlRow(kFooterRow1Y, false, kFooterDividerY, "Vol", vol_val, pan_val, "Pan");
            DrawControlRow(kFooterRow2Y, true, kFooterInterRowDividerY, "Vol/Pan*", "", "", "Rev/Dly");
        }
        else
        {
            // No channel currently lacks Pan besides Master (handled
            // above via is_master) -- kept as a real fallback rather
            // than an assert, in case that ever changes again.
            DrawControlRow(kFooterRow1Y, false, kFooterDividerY, "Vol", vol_val, "", "");
            DrawControlRow(kFooterRow2Y, true, kFooterInterRowDividerY, "Vol*", "", "", "Rev/Dly");
        }
    }
    else
    {
        char dly_val[8];
        snprintf(second_val, sizeof(second_val), "%d%%",
                  (int)(MixerGetSend01(sel_ch) * 100.f + 0.5f));
        snprintf(dly_val, sizeof(dly_val), "%d%%",
                  (int)(MixerGetDelaySend01(sel_ch) * 100.f + 0.5f));
        DrawControlRow(kFooterRow1Y, false, kFooterDividerY, "Rev", second_val, dly_val, "Dly");
        DrawControlRow(kFooterRow2Y, true, kFooterInterRowDividerY, "Vol/Pan", "", "", "Rev/Dly*");
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
    TomThumbDrawText(disp_, 0, 16, line1, true);

    // --- Reconstruct the signal-flow graph -------------------
    // Array index 0 = hardware OP6, index 5 = OP1 (confirmed from
    // the real DX7 SysEx spec), matching render()'s own
    // processing order (index 0..5) -- a bus can only ever be
    // read by an operator whose index is higher than whichever
    // operator(s) wrote it, so a single forward pass is enough to
    // resolve every edge and depth.
    int  input_bus[6], output_bus[6];
    bool sums[6], is_carrier[6] = {}, has_fb[6], fb_out[6];
    for(int i = 0; i < 6; i++)
    {
        FmOperatorRouting r = FmCore::get_operator_routing((uint8_t)algo0, i);
        input_bus[i]  = r.input_bus;
        output_bus[i] = r.output_bus;
        sums[i]       = r.sums;
        has_fb[i]     = r.has_feedback;
        fb_out[i]     = r.fb_out;
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
    // Rows count UP from the bottom, like the DX7's own diagrams: every
    // carrier sits on the same bottom row just above the OUT line, and
    // each modulator one row above the highest operator it feeds.
    // Children always have a higher index than their parents, so one
    // reverse pass settles every height.
    int height[6] = {};
    for(int i = 5; i >= 0; i--)
        for(int k = 0; k < edge_count[i]; k++)
        {
            int p = edge_from[i][k];
            if(height[i] + 1 > height[p])
                height[p] = height[i] + 1;
        }
    int max_height = 0;
    for(int i = 0; i < 6; i++)
        if(height[i] > max_height)
            max_height = height[i];
    int num_rows = max_height + 1;

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
    const int   kRowBase  = kOutLineY - 3; // carriers' bottom row ends here
    const float row_pitch = (float)(kRowBase - kDiagTop) / (float)num_rows;
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
        // Mirrored so the layout reads like the DX7's own diagrams:
        // columns are assigned in processing order (OP6 first), so
        // flipping puts OP1's chain on the left.
        cx[i] = (int)(col_pitch * ((float)(num_columns - 1) - column[i] + 0.5f));
        cy[i] = kRowBase - (int)(row_pitch * ((float)height[i] + 0.5f));
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
        // OP ON/OFF: a switched-off operator is a dotted, empty box
        // (whether carrier or modulator) so it reads as silent.
        bool op_off = dexed_ && !((dexed_->GetOpSwitch() >> i) & 1);
        if(op_off)
        {
            disp_->DrawRect(x0, y0, x1, y1, false, true);
            for(int x = x0; x <= x1; x += 2)
            {
                disp_->DrawPixel(x, y0, true);
                disp_->DrawPixel(x, y1, true);
            }
            for(int y = y0; y <= y1; y += 2)
            {
                disp_->DrawPixel(x0, y, true);
                disp_->DrawPixel(x1, y, true);
            }
        }
        else
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
        TomThumbDrawText(disp_, pen_x, pen_y, num, op_off || !is_carrier[i]);

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
        // A feedback loop over several operators (algorithm 4: OP6-OP4,
        // 6: OP6-OP5) is drawn after this loop instead.
        bool fb_loop = false;
        for(int k = i + 1; k < 6; k++)
            fb_loop = fb_loop || (has_fb[i] && fb_out[k]);
        if(has_fb[i] && !fb_loop)
        {
            const char* fb = "FB";
            int         fb_w = TomThumbInkWidth(fb);
            int         fb_x = x1 + 2;
            if(fb_x + fb_w > kDiagWidth)
                fb_x = x0 - 2 - fb_w;
            TomThumbDrawText(disp_, fb_x, pen_y, fb, true);
        }
    }

    // Multi-operator feedback: a vertical line beside the whole loop,
    // from the top of the operator that receives the feedback to the
    // bottom of the last one that sends it, with "FB" centred on it.
    for(int f = 0; f < 6; f++)
    {
        if(!has_fb[f])
            continue;
        int g = -1;
        for(int k = f + 1; k < 6; k++)
            if(fb_out[k])
                g = k;
        if(g < 0)
            continue;
        int top_y = cy[f] - box_h / 2, bot_y = cy[g] + box_h / 2;
        int right = cx[f];
        for(int k = f; k <= g; k++)
            if(cx[k] > right)
                right = cx[k];
        const char* fb   = "FB";
        int         fb_w = TomThumbInkWidth(fb);
        int         lx   = right + box_w / 2 + 2;
        int         tx   = lx + 2;
        if(tx + fb_w > kDiagWidth) // no room on the right -- go left
        {
            int left = cx[f];
            for(int k = f; k <= g; k++)
                if(cx[k] < left)
                    left = cx[k];
            lx = left - box_w / 2 - 2;
            tx = lx - 2 - fb_w;
        }
        disp_->DrawLine(lx, top_y, lx, bot_y, true);
        TomThumbDrawText(disp_, tx, (top_y + bot_y) / 2 + 2, fb, true);
    }
}

void Ui::DrawOscilloscope(int top, int bottom, const float* buf, size_t capacity, int cols)
{
    const int center_y = (top + bottom) / 2;
    const int half_h    = (bottom - top) / 2;

    if(!buf || capacity == 0)
    {
        disp_->DrawLine(1, center_y, 1 + (cols - 1) * 2, center_y, true);
        return;
    }

    const size_t stride = capacity / (size_t)cols;

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
    for(int col = 0; col < cols; col++)
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

void Ui::DrawMixPageScope(int top, const float* buf, size_t capacity, int mixer_ch)
{
    // Meter flush right, full body height; the scope stops 4px short of
    // it (58 columns end at x=115).
    constexpr int kMeterW = 6, kScopeCols = 58;
    const int     bottom  = kFooterDividerY - 2;
    DrawOscilloscope(top, bottom, buf, capacity, kScopeCols);
    DrawMixerVBar(disp_->Width() - kMeterW, top, kMeterW, bottom - top + 1,
                  MeterBar01(MixerGetLevel01(mixer_ch)), MixerIsClipping(mixer_ch));
}

void Ui::RefreshFileSlots()
{
    file_slot_count_ = PerformanceStore::ListSlots(file_slots_, kMaxFileSlots);
    // A card read error now shows here rather than as a shorter list
    // (see PerformanceStore::ScanSlotFolder()).
    if(PerformanceStore::GetLastError()[0] != '\0')
        snprintf(file_status_, sizeof(file_status_), "Read fail:%s",
                  PerformanceStore::GetLastError());
    if(file_cursor_ > file_slot_count_)
        file_cursor_ = file_slot_count_; // index 0 is "New"
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
    // The whole song: the sequencer's tracks and every synth engine (the
    // same capture as a Global:Snap save) ride along with the loops.
    // D2 is fine: Save() copies it to its own buffer before writing.
    static PerformanceStore::SynthSnapshot DSY_D2_BSS synths;
    CaptureSynthSnapshot(&synths);
    PerformanceStore::PerformanceExtrasOut extras;
    extras.seq          = seq_;
    extras.scenes       = scenes_;
    extras.arp          = arp_;
    extras.song         = &song_;
    extras.synths       = &synths;
    extras.granular_l   = granular_ ? granular_->GetSourceL() : nullptr;
    extras.granular_r   = granular_ ? granular_->GetSourceR() : nullptr;
    extras.granular_len = granular_ ? granular_->GetSourceLen() : 0;
    bool ok = PerformanceStore::Save(slot, *tempo_, layers_, num_layers_, master_volume01_,
                                       bypass_, master_filter_mode_, master_filter_cutoff01_,
                                       master_filter_res01_, reverb_size01_,
                                       bypass_reverb_send01_, reverb_predelay01_,
                                       reverb_prefilter01_, reverb_input_diffusion1_01_,
                                       reverb_input_diffusion2_01_, reverb_decay_diffusion01_,
                                       reverb_damping01_, GetDelayState(), &extras,
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

void Ui::SeqClearAll()
{
    if(!seq_)
        return;
    for(int t = 0; t < MidiSequencer::kTracks; t++)
        seq_->RequestClear(t);
    // The clears happen in the 1kHz control interrupt (which also lets go
    // of any notes they were playing) -- give it a couple of ticks.
    System::Delay(3);
}

void Ui::TriggerNew()
{
    for(int i = 0; i < num_layers_; i++)
        layers_[i].Clear();
    SeqClearAll();
    StopSong();
    StopSongRecord();
    for(Scene& s : scenes_)
        s = Scene{};
    scene_playing_ = scene_next_ = scene_edit_ = -1;
    song_        = Song{};
    song_cursor_ = 0;
    loaded_slot_ = -1; // next Save lands in a new slot, not over the old one
    snprintf(file_status_, sizeof(file_status_), "New perf");
}

void Ui::TriggerLoad(int slot)
{
    // Freeze the audio engine for the whole load: it restores layers one
    // at a time and each can take a while (streaming its recorded audio
    // off the SD card), and the audio ISR keeps running throughout a
    // blocking main-loop call like this one -- without suspending it, a
    // layer already restored earlier in Load() starts playing (and the
    // tempo clock keeps ticking) while later layers are still being
    // read, so every layer would resume at a different sample offset.
    // See audio_engine.h and TempoClock::ResetPhase().
    // Starts as the current values -- a v16+ file overwrites them, an
    // older one (no delay block) leaves them as they are.
    PerformanceStore::DelayState loaded_delay = GetDelayState();
    // The sequencer starts from empty tracks -- the file's own fill them
    // (a performance saved before the sequencer leaves them empty).
    StopSong();
    StopSongRecord();
    SeqClearAll();
    scene_playing_ = scene_next_ = scene_edit_ = -1;
    song_cursor_   = 0;
    // D2 is fine: Load() reads into its own buffer and copies it here.
    static PerformanceStore::SynthSnapshot DSY_D2_BSS synths;
    PerformanceStore::PerformanceExtrasIn  extras;
    extras.seq               = seq_;
    extras.scenes            = scenes_;
    extras.arp               = arp_;
    extras.song              = &song_;
    extras.synths            = &synths;
    extras.granular_l        = granular_capture_buf_l_;
    extras.granular_r        = granular_capture_buf_r_;
    extras.granular_capacity = granular_capture_capacity_;
    g_seq_hold           = true; // the sequencer/arps start over after it
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
                                       &loaded_delay, &extras, &Ui::OnSaveLoadProgress);
    file_op_in_progress_ = false;
    g_progress_disp       = nullptr;

    // The engines it was saved with (an older performance has none and
    // leaves them as they are) -- still suspended, and the sequencer held:
    // applying them stops every engine's notes and can read a sample kit
    // off the card, which with the clock running cut into bar 1.
    bool kit_missing = false;
    if(ok)
    {
        ApplyDelayState(loaded_delay);
        kit_missing = extras.synths_loaded && ApplySynthSnapshot(synths, extras.granular_len);
    }
    g_audio_suspended = false; // every layer, the tempo's phase and the engines are ready now
    g_seq_hold        = false;

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
        snprintf(file_status_, sizeof(file_status_),
                  kit_missing ? "PERF%d: kit missing" : "Loaded PERF%d", slot);
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
        case SdMgmtFolder::Global:
            if(SdMgmtSnapshots())
            {
                if(snap_slots_dirty_)
                    RefreshSnapSlots();
                *out_count = snap_slot_count_;
                return snap_slots_;
            }
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
            if(sd_mgmt_ens_kind_ == 2)
            {
                if(ssw_slots_dirty_)
                    RefreshSupersawPresetSlots();
                *out_count = ssw_user_count_;
                return ssw_user_slots_;
            }
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

void Ui::SdMgmtOpenBox()
{
    // Force a fresh scan (same "re-scan on entry" idiom as Global:File)
    // since files may have changed since the last visit.
    sd_mgmt_in_folder_ = true;
    sd_mgmt_cursor_    = 0;
    sd_mgmt_status_[0] = '\0';
    switch(sd_mgmt_folder_)
    {
        case SdMgmtFolder::Global:
            file_slots_dirty_    = true;
            snap_slots_dirty_    = true;
            sd_mgmt_global_kind_ = -1; // Perf/Snapshots choice first
            break;
        case SdMgmtFolder::GranularPresets: granular_preset_slots_dirty_ = true; break;
        case SdMgmtFolder::DexedPresets:
            dexed_preset_slots_dirty_ = true;
            dexed_browse_name_slot_   = -2; // a slot number may now hold a different save
            break;
        case SdMgmtFolder::EnsemblePresets:
            ensemble_preset_slots_dirty_ = true;
            sd_mgmt_ens_kind_            = -1; // Ensemble/Organ choice first
            break;
        case SdMgmtFolder::SampleSynthCaptures:
        case SdMgmtFolder::SampleSynthImports:
            sd_mgmt_capt_kind_     = -1; // CAPT: Sampler/Grains choice first
            sd_mgmt_smp_kind_      = -1; // SMPIMP: Voice/Drum choice first
            sd_mgmt_smp_sub_kind_ = -1;
            sd_mgmt_drum_kind_   = -1; // SMPDR: Kits/Breaks choice first
            sd_mgmt_bank_        = -1; // bank list first
            sd_mgmt_banks_dirty_ = true;
            break;
        case SdMgmtFolder::SampleSynthVoiceUserPresets: sample_synth_voice_user_dirty_ = true; break;
        case SdMgmtFolder::SampleSynthDrumUserPresets:
            sample_synth_drum_user_dirty_ = true;
            sd_mgmt_drum_kind_            = -1; // Kits/Breaks choice first
            break;
        default: break;
    }
}

void Ui::RefreshSdMgmtBanks()
{
    using PerformanceStore::SampleBank;
    if(SdMgmtGrainsCaptures())
    {
        // Grains banks reuse SampleBankEntry for the number only -- every
        // Grains branch below goes by SdMgmtGrainsCaptures(), not .type.
        uint8_t numbers[PerformanceStore::kMaxGranularCapturedBanks];
        int     n = PerformanceStore::ListGranularCapturedBanks(
            numbers, PerformanceStore::kMaxGranularCapturedBanks);
        if(n > kMaxListedSampleBanks)
            n = kMaxListedSampleBanks;
        for(int i = 0; i < n; i++)
        {
            sd_mgmt_banks_[i].type   = SampleBank::Captured;
            sd_mgmt_banks_[i].number = numbers[i];
            sd_mgmt_banks_[i].items  = -1; // a Grains bank -- not a SAMPLES/ folder
        }
        sd_mgmt_bank_count_ = n;
    }
    else if(sd_mgmt_folder_ == SdMgmtFolder::SampleSynthImports)
    {
        // The banks of whichever kind was picked: Voice -> Sounds or Multi,
        // Drum -> Kits or Breaks.
        SampleBank type = sd_mgmt_smp_kind_ == 0
                              ? (sd_mgmt_smp_sub_kind_ == 1 ? SampleBank::Multi : SampleBank::Sounds)
                              : (sd_mgmt_smp_sub_kind_ == 1 ? SampleBank::Breaks : SampleBank::Kits);
        uint8_t numbers[PerformanceStore::kMaxSampleBanks];
        int     n = PerformanceStore::ListSampleBanks(type, numbers, PerformanceStore::kMaxSampleBanks);
        int     count = 0;
        for(int i = 0; i < n && count < kMaxListedSampleBanks; i++, count++)
        {
            sd_mgmt_banks_[count].type   = type;
            sd_mgmt_banks_[count].number = numbers[i];
            sd_mgmt_banks_[count].items  = (int8_t)PerformanceStore::SampleBankItemCount(type, numbers[i]);
        }
        sd_mgmt_bank_count_ = count;
    }
    else
    {
        uint8_t numbers[PerformanceStore::kMaxSampleBanks];
        int n = PerformanceStore::ListSampleBanks(SampleBank::Captured, numbers,
                                                  PerformanceStore::kMaxSampleBanks);
        for(int i = 0; i < n; i++)
        {
            sd_mgmt_banks_[i].type   = SampleBank::Captured;
            sd_mgmt_banks_[i].number = numbers[i];
            sd_mgmt_banks_[i].items
                = (int8_t)PerformanceStore::SampleBankItemCount(SampleBank::Captured, numbers[i]);
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
        if(SdMgmtGrainsCaptures())
        {
            char folder[16];
            snprintf(folder, sizeof(folder), "CAPTURED%d", e.number);
            sd_mgmt_item_count_ = PerformanceStore::ListGranularLibraryItems(
                folder, sd_mgmt_item_names_, kMaxSampleSynthSoundsListed);
        }
        else
            sd_mgmt_item_count_ = PerformanceStore::ListSampleBankItems(
                e.type, e.number, sd_mgmt_item_names_, kMaxSampleSynthSoundsListed);
    }
    sd_mgmt_items_dirty_ = false;
}

int Ui::SdMgmtListCount()
{
    if(SdMgmtGlobalBox() && sd_mgmt_global_kind_ < 0)
        return 2; // Perf / Snapshots
    if(SdMgmtEnsembleBox() && sd_mgmt_ens_kind_ < 0)
        return 3; // Ensemble / Organ / Supersaw
    if(SdMgmtDrumUserBox())
    {
        int n = 0;
        SdMgmtSlots(&n); // refreshes, which also sorts saves into Kits/Breaks
        return sd_mgmt_drum_kind_ < 0 ? 2
                                      : sample_synth_drum_user_folder_count_[sd_mgmt_drum_kind_];
    }
    if(SdMgmtIsBankFolder())
        return SdMgmtBankLevelCount();
    int n = 0;
    SdMgmtSlots(&n);
    return n;
}

int Ui::SdMgmtBankLevelCount()
{
    if(sd_mgmt_folder_ == SdMgmtFolder::SampleSynthCaptures && sd_mgmt_capt_kind_ < 0)
        return 2; // Sampler, Grains
    if(SdMgmtSmpChoosing())
        return 2; // Voice/Drum, then Sounds/Multi or Kits/Breaks
    if(sd_mgmt_banks_dirty_)
        RefreshSdMgmtBanks();
    if(sd_mgmt_bank_ < 0)
        return sd_mgmt_bank_count_;
    if(sd_mgmt_items_dirty_)
        RefreshSdMgmtItems();
    return sd_mgmt_item_count_;
}

void Ui::SourceDisplayName(const char* src, char* out, size_t out_size)
{
    const char* base = strrchr(src, '/');
    base             = base ? base + 1 : src;
    snprintf(out, out_size, "%s", base);
    char* dot = strrchr(out, '.');
    if(dot && dot != out)
        *dot = '\0';
}

void Ui::SampleSynthUserSaveName(bool voice, int i, char* out, size_t out_size) const
{
    const char* name = voice ? sample_synth_voice_user_names_[i] : sample_synth_drum_user_names_[i];
    if(name[0] != '\0')
        snprintf(out, out_size, "%s", name);
    else
        snprintf(out, out_size, "%s%d", voice ? "SMPVO" : "SMPDR",
                  voice ? sample_synth_voice_user_numbers_[i] : sample_synth_drum_user_numbers_[i]);
}

void Ui::RefreshSampleSynthVoiceUserSlots()
{
    sample_synth_voice_user_count_ = PerformanceStore::ListSampleSynthVoiceUserPresets(
        sample_synth_voice_user_numbers_, PerformanceStore::kMaxSampleSynthVoiceUserPresets);
    // Each save's name -- one small read per save, only on refresh.
    for(int i = 0; i < sample_synth_voice_user_count_; i++)
    {
        static PerformanceStore::SampleSynthVoiceUserPreset p; // AXI, not stack -- see Save()'s DMA comment
        sample_synth_voice_user_names_[i][0] = '\0';
        if(PerformanceStore::LoadSampleSynthVoiceUserPreset(sample_synth_voice_user_numbers_[i], &p))
            SourceDisplayName(p.filename, sample_synth_voice_user_names_[i], kUserSaveNameLen);
    }
    if(sample_synth_voice_user_cursor_ >= sample_synth_voice_user_count_)
        sample_synth_voice_user_cursor_
            = sample_synth_voice_user_count_ > 0 ? sample_synth_voice_user_count_ - 1 : 0;
    sample_synth_voice_user_dirty_ = false;
    if(sd_mgmt_cursor_ >= sample_synth_voice_user_count_)
        sd_mgmt_cursor_
            = sample_synth_voice_user_count_ > 0 ? sample_synth_voice_user_count_ - 1 : 0;
}

int Ui::DrumUserGlobalIndex(int folder, int k) const
{
    for(int i = 0; i < sample_synth_drum_user_count_; i++)
        if((sample_synth_drum_user_is_break_[i] ? 1 : 0) == folder && k-- == 0)
            return i;
    return -1;
}

int Ui::DrumUserFolderPos(int global) const
{
    int pos = 0;
    for(int i = 0; i < global && i < sample_synth_drum_user_count_; i++)
        if(sample_synth_drum_user_is_break_[i] == sample_synth_drum_user_is_break_[global])
            pos++;
    return pos;
}

void Ui::RefreshSampleSynthDrumUserSlots()
{
    sample_synth_drum_user_count_ = PerformanceStore::ListSampleSynthDrumUserPresets(
        sample_synth_drum_user_numbers_, PerformanceStore::kMaxSampleSynthDrumUserPresets);
    // Sort each save into Kits or Breaks by the kit folder it points at --
    // one small (100-byte) read per save, only on refresh.
    sample_synth_drum_user_folder_count_[0] = sample_synth_drum_user_folder_count_[1] = 0;
    for(int i = 0; i < sample_synth_drum_user_count_; i++)
    {
        static PerformanceStore::SampleSynthDrumUserPreset p; // AXI, not stack -- see Save()'s DMA comment
        bool is_break = PerformanceStore::LoadSampleSynthDrumUserPreset(
                            sample_synth_drum_user_numbers_[i], &p)
                        && strncmp(p.kit_ref, "BREAKS", 6) == 0;
        sample_synth_drum_user_is_break_[i] = is_break;
        sample_synth_drum_user_names_[i][0] = '\0';
        if(is_break || p.kit_ref[0] != '\0')
            SourceDisplayName(p.kit_ref, sample_synth_drum_user_names_[i], kUserSaveNameLen);
        sample_synth_drum_user_folder_count_[is_break ? 1 : 0]++;
    }
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
    bool multi = sd_mgmt_banks_[sd_mgmt_bank_].type == PerformanceStore::SampleBank::Multi;
    if(sd_mgmt_folder_ == SdMgmtFolder::SampleSynthImports && sd_mgmt_smp_kind_ == 0 && !multi)
    {
        // An imported Sound -- copied into the same bank as "<name> 2".
        ok = PerformanceStore::DuplicateSampleSound(folder, item, result, sizeof(result),
                                                    &Ui::OnSaveLoadProgress);
    }
    else if(SdMgmtGrainsCaptures())
    {
        // Stays in the same folder, like the Sampler's own captures.
        int n = CaptureNumberFromName(item), new_n = -1;
        ok    = n > 0
             && PerformanceStore::DuplicateGranularCapture(sd_mgmt_banks_[sd_mgmt_bank_].number, n,
                                                           &new_n, &Ui::OnSaveLoadProgress);
        if(ok)
            snprintf(result, sizeof(result), "CAP%03d", new_n);
    }
    else if(sd_mgmt_folder_ == SdMgmtFolder::SampleSynthCaptures)
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
        granular_lib_dirty_       = true;
        granular_lib_items_dirty_ = true;
        sd_mgmt_items_dirty_      = true;
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
    bool multi = sd_mgmt_banks_[sd_mgmt_bank_].type == PerformanceStore::SampleBank::Multi;
    if(sd_mgmt_folder_ == SdMgmtFolder::SampleSynthImports && sd_mgmt_smp_kind_ == 0 && !multi)
    {
        // An imported Sound.
        bool was_loaded = strcmp(sample_synth_loaded_sound_category_, folder) == 0
                          && strcmp(sample_synth_loaded_sound_name_, item) == 0;
        ok = PerformanceStore::DeleteSampleSound(folder, item);
        if(ok && was_loaded)
            sample_synth_loaded_sound_name_[0] = '\0';
    }
    else if(SdMgmtGrainsCaptures())
    {
        // Same "only the exact loaded file" rule as the Sampler's below.
        bool was_loaded = strcmp(granular_loaded_lib_folder_, folder) == 0
                          && strcmp(granular_loaded_lib_name_, item) == 0;
        int n = CaptureNumberFromName(item);
        ok    = n > 0
             && PerformanceStore::DeleteGranularCapture(sd_mgmt_banks_[sd_mgmt_bank_].number, n);
        if(ok && was_loaded)
        {
            granular_loaded_lib_name_[0]   = '\0';
            granular_loaded_lib_folder_[0] = '\0';
        }
    }
    else if(sd_mgmt_folder_ == SdMgmtFolder::SampleSynthCaptures)
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
        // A Kit, Break or Multi folder.
        bool was_loaded = multi && strcmp(sample_synth_loaded_sound_category_, folder) == 0
                          && strcmp(sample_synth_loaded_sound_name_, item) == 0;
        ok = PerformanceStore::DeleteSampleSynthKit(ref);
        if(ok && was_loaded)
            sample_synth_loaded_sound_name_[0] = '\0';
        if(ok && strcmp(sample_synth_loaded_kit_ref_, ref) == 0)
            sample_synth_loaded_kit_ref_[0] = '\0';
        if(ok && strcmp(sample_synth_drum_kit_ref_, ref) == 0)
            sample_synth_drum_kit_ref_[0] = '\0'; // nothing left to restore on a mode switch
    }
    if(ok)
    {
        snprintf(sd_mgmt_status_, sizeof(sd_mgmt_status_), "Deleted:%s", item);
        MarkSampleSynthListsDirty();
        granular_lib_dirty_       = true;
        granular_lib_items_dirty_ = true;
        sd_mgmt_items_dirty_      = true;
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
    int idx = SdMgmtItemIndex();
    if(!slots || idx < 0 || idx >= count)
        return;
    int  slot     = slots[idx];
    int  new_slot = -1;
    bool ok;
    g_progress_disp = disp_;
    switch(sd_mgmt_folder_)
    {
        case SdMgmtFolder::Global:
            ok = SdMgmtSnapshots()
                     ? PerformanceStore::DuplicateSynthSnapshot(slot, &new_slot,
                                                                &Ui::OnSaveLoadProgress)
                     : PerformanceStore::DuplicateSlot(slot, &new_slot, &Ui::OnSaveLoadProgress);
            break;
        case SdMgmtFolder::GranularPresets:
            ok = PerformanceStore::DuplicateGranularPreset(slot, &new_slot,
                                                              &Ui::OnSaveLoadProgress);
            break;
        case SdMgmtFolder::DexedPresets:
            ok = PerformanceStore::DuplicateDexedPreset(slot, &new_slot, &Ui::OnSaveLoadProgress);
            break;
        case SdMgmtFolder::EnsemblePresets:
            ok = sd_mgmt_ens_kind_ == 2
                     ? PerformanceStore::DuplicateSupersawPreset(slot, &new_slot,
                                                                 &Ui::OnSaveLoadProgress)
                     : PerformanceStore::DuplicateEnsemblePreset(slot, &new_slot,
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
                  SdMgmtItemPrefix(), new_slot);
        switch(sd_mgmt_folder_)
        {
            case SdMgmtFolder::Global:
                file_slots_dirty_ = true;
                snap_slots_dirty_ = true;
                break;
            case SdMgmtFolder::GranularPresets: granular_preset_slots_dirty_ = true; break;
            case SdMgmtFolder::DexedPresets:
                dexed_preset_slots_dirty_ = true;
                dexed_browse_name_slot_   = -2; // a slot number may now hold a different save
                break;
            case SdMgmtFolder::EnsemblePresets:
                ensemble_preset_slots_dirty_ = true;
                ssw_slots_dirty_             = true;
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
    int idx = SdMgmtItemIndex();
    if(!slots || idx < 0 || idx >= count)
        return;
    int  slot = slots[idx];
    bool ok;
    // Passing the relevant page's own "currently loaded" tracker lets
    // Delete*() keep it correct across the gap-closing renumbering it
    // does (see its own doc comment) -- otherwise a delete elsewhere in
    // the list could silently leave e.g. the File page's "Now: N" naming
    // the wrong performance (or Overwrite writing to it) after everything
    // above it shifts down.
    switch(sd_mgmt_folder_)
    {
        case SdMgmtFolder::Global:
            ok = SdMgmtSnapshots() ? PerformanceStore::DeleteSynthSnapshot(slot, &snap_loaded_slot_)
                                   : PerformanceStore::DeleteSlot(slot, &loaded_slot_);
            break;
        case SdMgmtFolder::GranularPresets:
            ok = PerformanceStore::DeleteGranularPreset(slot, &granular_loaded_preset_slot_);
            break;
        case SdMgmtFolder::DexedPresets:
            ok = PerformanceStore::DeleteDexedPreset(slot, &dexed_loaded_preset_slot_);
            break;
        case SdMgmtFolder::EnsemblePresets:
            ok = sd_mgmt_ens_kind_ == 2
                     ? PerformanceStore::DeleteSupersawPreset(slot, &ssw_loaded_slot_)
                     : PerformanceStore::DeleteEnsemblePreset(slot, &ensemble_loaded_preset_slot_);
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
                  SdMgmtItemPrefix(), slot);
        switch(sd_mgmt_folder_)
        {
            case SdMgmtFolder::Global:
                file_slots_dirty_ = true;
                snap_slots_dirty_ = true;
                break;
            case SdMgmtFolder::GranularPresets: granular_preset_slots_dirty_ = true; break;
            case SdMgmtFolder::DexedPresets:
                dexed_preset_slots_dirty_ = true;
                dexed_browse_name_slot_   = -2; // a slot number may now hold a different save
                break;
            case SdMgmtFolder::EnsemblePresets:
                ensemble_preset_slots_dirty_ = true;
                ssw_slots_dirty_             = true;
                break;
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
    // A fresh capture isn't saved anywhere yet. Loads and New call this
    // too, then MarkGranularClean() straight after, which clears it again.
    granular_capture_unsaved_      = full_len > 0;
    granular_loaded_lib_name_[0]   = '\0';
    granular_loaded_lib_folder_[0] = '\0';
    granular_trim_status_[0]       = '\0';
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
    SelectSampleFace(target);
    // Preset/Import's own Kits-vs-Sounds split now just follows the
    // face directly -- see that field's own comment.
    sample_synth_preset_mode_sounds_ = (target == SampleSynthMode::Voice);
    sample_synth_import_dirty_       = true; // the Import page follows the face
    sample_synth_import_drum_open_   = false;
    sample_synth_kit_cursor_         = 0;
    sample_synth_sound_category_     = -1;
    sample_synth_voice_type_         = -1; // the other face has its own types
    MarkSampleSynthListsDirty();
    sample_synth_voice_user_dirty_   = true;
    sample_synth_drum_user_dirty_    = true;
}

void Ui::SelectSampleFace(SampleSynthMode target)
{
    if(!sample_synth_ || target == sample_synth_mode_)
        return;
    // Voice and Drum are separate engines with separate arenas now, so
    // nothing is snapshotted or reloaded here -- the pages just point at
    // the other engine, and the per-face UI state swaps with it.
    if(sample_synth_mode_ == SampleSynthMode::Drum)
        sample_synth_drum_last_page_ = sample_synth_param_page_;
    else
        sample_synth_voice_last_page_ = sample_synth_param_page_;
    ParkSampleFace();

    sample_synth_mode_ = target;
    const int face     = target == SampleSynthMode::Drum ? 1 : 0;
    sample_synth_        = sample_synth_engines_[face];
    sample_synth_arena_l_ = sample_synth_arenas_[face][0];
    sample_synth_arena_r_ = sample_synth_arenas_[face][1];
    // Restore whichever page THIS face was last left on (Filter by
    // default, the first time it's ever visited) -- NOT a forced
    // reset to Filter on every switch (direct correction: only on
    // startup/first visit, not every time after). See
    // sample_synth_voice_last_page_/_drum_last_page_'s own comment.
    sample_synth_param_page_ = target == SampleSynthMode::Voice
                                    ? sample_synth_voice_last_page_
                                    : sample_synth_drum_last_page_;
}

void Ui::ParkSampleFace()
{
    SampleFaceState& p = sample_parked_face_;
    std::swap(sample_synth_loaded_kit_ref_, p.loaded_kit_ref);
    std::swap(sample_synth_loaded_kit_name_, p.loaded_kit_name);
    std::swap(sample_synth_kit_status_, p.kit_status);
    std::swap(sample_synth_loaded_sound_name_, p.loaded_sound_name);
    std::swap(sample_synth_loaded_sound_category_, p.loaded_sound_category);
    std::swap(sample_synth_kit_real_slots_, p.real_slots);
    std::swap(sample_synth_kit_real_pad_len_, p.real_pad_len);
    std::swap(sample_synth_kit_real_pad_count_, p.real_pad_count);
    std::swap(sample_synth_kit_real_total_len_, p.real_total_len);
    std::swap(sample_synth_setup_tune_semis_, p.setup_tune_semis);
    std::swap(sample_synth_setup_pending_slices_, p.setup_pending_slices);
    std::swap(sample_synth_setup_applied_slices_, p.setup_applied_slices);
}

void Ui::ReloadSampleFace(SampleSynthMode face)
{
    if(!sample_synth_)
        return;
    const SampleSynthMode shown      = sample_synth_mode_;
    const EditSnapshot    keep_voice = sample_synth_voice_baseline_;
    const EditSnapshot    keep_drum  = sample_synth_drum_baseline_;
    SelectSampleFace(face);
    if(face == SampleSynthMode::Drum)
    {
        const int    slices = sample_synth_setup_applied_slices_;
        const int8_t tune   = sample_synth_setup_tune_semis_;
        sample_synth_->ResetAllVoices();
        for(int i = 0; i < SampleSynth::kMaxSampleSlots; i++)
            sample_synth_->ClearSlot(i);
        sample_synth_kit_real_pad_count_ = 0;
        if(sample_synth_drum_kit_ref_[0] != '\0')
        {
            // Copied first -- LoadSampleSynthKitByRef() rewrites
            // sample_synth_drum_kit_ref_ itself on success.
            char ref[PerformanceStore::kMaxSampleSynthKitRefLen];
            snprintf(ref, sizeof(ref), "%s", sample_synth_drum_kit_ref_);
            LoadSampleSynthKitByRef(ref);
            // A fresh Kit load resets Setup's Break Divide/Tune -- put
            // back whatever was active.
            sample_synth_setup_pending_slices_ = slices;
            sample_synth_setup_tune_semis_     = tune;
            ApplySampleSynthBreakDivide(slices, tune);
            sample_synth_setup_applied_slices_ = slices;
        }
    }
    else if(sample_synth_voice_source_ == SampleSynthVoiceSource::Imported)
    {
        // A capture lives in its own buffer and survives; only an
        // imported Sound sits in the arena.
        sample_synth_->ResetAllVoices();
        for(int i = 0; i < SampleSynth::kMaxSampleSlots; i++)
            sample_synth_->ClearSlot(i);
        if(sample_synth_voice_sound_name_[0] != '\0')
            LoadSampleSynthFileIntoEngine(sample_synth_voice_sound_category_,
                                            sample_synth_voice_sound_name_);
        for(int si = 0; si < SampleSynth::kMaxSampleSlots; si++)
            if(sample_synth_->SlotHasSample(si))
                sample_synth_->SetSlotPitchOffsetSemis(si, sample_synth_imp_tune_semis_);
    }
    SelectSampleFace(shown);
    sample_synth_voice_baseline_ = keep_voice;
    sample_synth_drum_baseline_  = keep_drum;
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

void Ui::RefreshSampleSynthImport()
{
    using PerformanceStore::SampleBank;
    bool voice = sample_synth_mode_ == SampleSynthMode::Voice;
    if(!voice)
        PerformanceStore::EnsureDrumImportFolders();
    sample_synth_import_sounds_ = sample_synth_import_kits_ = 0;
    sample_synth_import_breaks_ = sample_synth_import_loose_ = 0;

    // Count every .aif waiting, 32 at a time.
    static char DSY_D2_BSS names[PerformanceStore::kMaxSampleSynthImportPackFiles]
                     [PerformanceStore::kMaxSampleSynthWavNameLen + 1];
    auto count = [&](const char* folder, int& out) {
        for(int skip = 0;;)
        {
            int n = PerformanceStore::ListAifFiles(
                folder, names, PerformanceStore::kMaxSampleSynthImportPackFiles, skip);
            out += n;
            skip += n;
            if(n < PerformanceStore::kMaxSampleSynthImportPackFiles)
                break;
        }
    };
    if(voice)
        count(PerformanceStore::kSampleSoundImportFolder, sample_synth_import_sounds_);
    else
    {
        count(PerformanceStore::kSampleKitImportFolder, sample_synth_import_kits_);
        count(PerformanceStore::kSampleBreakImportFolder, sample_synth_import_breaks_);
        count(PerformanceStore::kSampleDrumImportFolder, sample_synth_import_loose_);
        count(PerformanceStore::kSampleLegacyBreakImportFolder, sample_synth_import_loose_);
    }

    // Destinations: New bank first, then every bank of the type being
    // imported with room for ALL of it.
    SampleBank type = SampleSynthImportType();
    int        need = voice                              ? sample_synth_import_sounds_
                      : sample_synth_import_drum_breaks_ ? sample_synth_import_breaks_
                                                         : sample_synth_import_kits_;
    sample_synth_import_dest_count_ = 1;
    sample_synth_import_dests_[0]   = ImportDest{};
    if(need > 0)
    {
        uint8_t numbers[PerformanceStore::kMaxSampleBanks];
        int     nb = PerformanceStore::ListSampleBanks(type, numbers,
                                                       PerformanceStore::kMaxSampleBanks);
        for(int i = 0; i < nb; i++)
        {
            int free = PerformanceStore::kSampleBankCapacity
                       - PerformanceStore::SampleBankItemCount(type, numbers[i]);
            if(free >= need)
                sample_synth_import_dests_[sample_synth_import_dest_count_++]
                    = ImportDest{numbers[i], free};
        }
    }
    if(sample_synth_import_dest_cursor_ >= sample_synth_import_dest_count_)
        sample_synth_import_dest_cursor_ = 0;
    sample_synth_import_dirty_ = false;
}

void Ui::TriggerSampleSynthPackImport()
{
    using PerformanceStore::SampleBank;
    if(!SampleSynthImportReady())
        return;
    if(sample_synth_import_dirty_)
        RefreshSampleSynthImport();
    bool       voice   = sample_synth_mode_ == SampleSynthMode::Voice;
    bool       breaks  = !voice && sample_synth_import_drum_breaks_;
    SampleBank type    = SampleSynthImportType();
    int        waiting = voice    ? sample_synth_import_sounds_
                         : breaks ? sample_synth_import_breaks_
                                  : sample_synth_import_kits_;
    if(waiting == 0)
        return;

    // Where it goes -- see RefreshSampleSynthImport(). "New bank" is one
    // above the type's highest bank.
    PerformanceStore::ClearImportDestinations();
    const ImportDest& dest = sample_synth_import_dests_[sample_synth_import_dest_cursor_];
    int bank = dest.number > 0 ? dest.number : PerformanceStore::NextNewSampleBank(type);
    if(bank > 0)
        PerformanceStore::SetImportDestination(type, bank);

    int ok_count = 0, skip_count = 0;
    g_progress_disp = disp_; // same "WORKING..." overlay every other multi-step SD operation uses
    // Draw the overlay immediately -- the first file can take a real
    // amount of time (AIFF decode + resample + up to 24 slice writes).
    OnSaveLoadProgress(0.f);

    // Imports every .aif in one staging folder, however many -- listed
    // kMaxSampleSynthImportPackFiles at a time and re-listed until none
    // are left. A file that fails is retried once straight away (some
    // failures are transient -- a file that skipped would import fine
    // on the next press), then left in place for the next press; later
    // passes step past it (ListAifFiles()'s skip_first) so it can never
    // block the files behind it. Success moves the file into Done/.
    auto sweep = [&](const char* folder) {
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
                    ok = voice ? PerformanceStore::ImportAifSamplerPack(
                             names[i], folder, sample_synth_scratch_l_, sample_synth_scratch_r_,
                             sample_synth_arena_capacity_, status, sizeof(status),
                             &Ui::OnSampleSynthImportFileProgress)
                               : PerformanceStore::ImportAifDrumPack(
                                   names[i], folder, breaks, sample_synth_scratch_l_,
                                   sample_synth_scratch_r_, sample_synth_arena_capacity_, status,
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

    // The decode scratch IS both engines' arenas (see
    // sample_synth_scratch_l_), so nothing may play from them meanwhile
    // -- empty every arena-backed slot now, reload both faces after.
    for(int f = 0; f < 2; f++)
    {
        SampleSynth* eng = sample_synth_engines_[f];
        if(!eng)
            continue;
        eng->ResetAllVoices();
        if(f == 1 || sample_synth_voice_source_ != SampleSynthVoiceSource::Captured)
            for(int i = 0; i < SampleSynth::kMaxSampleSlots; i++)
                eng->ClearSlot(i);
    }
    sweep(voice    ? PerformanceStore::kSampleSoundImportFolder
          : breaks ? PerformanceStore::kSampleBreakImportFolder
                   : PerformanceStore::kSampleKitImportFolder);
    PerformanceStore::ClearImportDestinations();
    ReloadSampleFace(SampleSynthMode::Voice);
    ReloadSampleFace(SampleSynthMode::Drum);
    if(ok_count > 0)
        MarkSampleSynthListsDirty();
    sample_synth_import_dirty_ = true; // what's waiting has changed
    g_progress_disp            = nullptr;

    // Where it went, e.g. "7 -> Sounds 14" (plus any that failed).
    SampleBankEntry e;
    e.type   = type;
    e.number = (uint8_t)(bank > 0 ? bank : 0);
    char where[32];
    SampleBankEntryLabel(e, where, sizeof(where));
    if(skip_count > 0)
        snprintf(sample_synth_pack_import_status_, sizeof(sample_synth_pack_import_status_),
                  "%d -> %s, %d failed", ok_count, where, skip_count);
    else
        snprintf(sample_synth_pack_import_status_, sizeof(sample_synth_pack_import_status_),
                  "%d -> %s", ok_count, where);
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
        case PerformanceStore::SampleBank::Multi: name = "Multi"; break;
    }
    if(e.items >= 0)
        snprintf(out, out_size, "%s %d (%d free)", name, e.number,
                  PerformanceStore::kSampleBankCapacity - e.items);
    else
        snprintf(out, out_size, "%s %d", name, e.number);
}

int Ui::ListFaceSampleBanks(bool drum, SampleBankEntry* out, int max_out, int type)
{
    using PerformanceStore::SampleBank;
    SampleBank t = FaceBankType(drum, type);
    uint8_t    numbers[PerformanceStore::kMaxSampleBanks];
    int        n     = PerformanceStore::ListSampleBanks(t, numbers, PerformanceStore::kMaxSampleBanks);
    int        count = 0;
    for(int i = 0; i < n && count < max_out; i++, count++)
    {
        out[count].type   = t;
        out[count].number = numbers[i];
        out[count].items  = (int8_t)PerformanceStore::SampleBankItemCount(t, numbers[i]);
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

void Ui::SampleLoadRetryPause(int attempt)
{
    // Back-to-back retries can all land inside the same brief SD hiccup
    // (seen on hardware: all 3 tries of a long break failed, then it
    // loaded fine moments later), so give the card a moment first, and
    // re-mount it before the last try.
    if(attempt <= 0)
        return;
    System::Delay(50);
    if(attempt == 2)
        PerformanceStore::Remount();
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
    // Restored as found: a Performance load calls this with the audio
    // already suspended, and must stay so until it's done (see TriggerLoad()).
    const bool was_suspended = g_audio_suspended;
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
        int    load_failed  = 0;
        char   load_fail_err[16] = {};
        int    no_room           = 0; // pads past the arena's ~20s
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
                no_room++;
                continue;
            }
            // Up to 3 tries per pad: an SD read here fails now and then
            // (measured: 17-18 of 20 pads loaded, a different count each
            // try, every loaded pad byte-exact) -- the same one-off
            // failures the pack importer already retries past.
            size_t len     = 0;
            bool   file_ok = false;
            for(int attempt = 0; attempt < 3 && !(file_ok && len > 0); attempt++)
            {
                SampleLoadRetryPause(attempt);
                len     = 0;
                file_ok = PerformanceStore::ImportWav16(slots[i].filename,
                                                      sample_synth_arena_l_ + cursor,
                                                      sample_synth_arena_r_ + cursor,
                                                      sample_synth_arena_capacity_ - cursor, &len,
                                                      nullptr, kit_dir);
            }
            if(!file_ok || len == 0)
            {
                // Still failing after 3 tries -- keep the SD error for the
                // status line (see below).
                snprintf(load_fail_err, sizeof(load_fail_err), "%s",
                          PerformanceStore::GetLastError());
                load_failed++;
                sample_synth_->ClearSlot(i);
                continue;
            }
            if(len >= sample_synth_arena_capacity_ - cursor)
            {
                // Filled what was left, so it was cut short -- drop it
                // rather than play a clipped sound; a shorter pad later
                // in the kit may still fit.
                no_room++;
                sample_synth_->ClearSlot(i);
                continue;
            }
            sample_synth_->LoadSlot16(i, sample_synth_arena_l_ + cursor,
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
        if(load_failed > 0)
            snprintf(sample_synth_kit_status_, sizeof(sample_synth_kit_status_), "%d/%d %s",
                      loaded_count, loaded_count + load_failed + no_room, load_fail_err);
        else if(no_room > 0)
            snprintf(sample_synth_kit_status_, sizeof(sample_synth_kit_status_),
                      "Loaded %d, %d no room", loaded_count, no_room);
        else
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
    g_audio_suspended = was_suspended;
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
            sample_synth_->LoadSlot16(j, sample_synth_arena_l_ + offset,
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
            sample_synth_->LoadSlot16(j, sample_synth_arena_l_ + start, sample_synth_arena_r_ + start,
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
    // Restored as found: a Performance load calls this with the audio
    // already suspended, and must stay so until it's done (see TriggerLoad()).
    const bool was_suspended = g_audio_suspended;
    g_audio_suspended = true;
    // Kill every voice before any slot gets reassigned -- see
    // SampleSynth::ResetAllVoices()'s own doc comment for why (a voice
    // still sounding from before would otherwise jump straight to the
    // new audio with no NoteOn at all).
    sample_synth_->ResetAllVoices();

    if(PerformanceStore::SampleSynthKitBank(category_folder) == PerformanceStore::SampleBank::Multi)
    {
        // Multi-sample melodic instrument -- a MULTISMPn folder holding a
        // recording per sampled note, each its own zone, so every note
        // plays from its nearest real pitch instead of one sample
        // stretched across the whole keyboard. See PerformanceStore::
        // BuildMultiSampleZones(). Bump-allocates each zone's WAV into
        // the shared arena exactly like TriggerLoadSampleSynthKit()
        // already does.
        PerformanceStore::SampleSynthKitSlot slots[PerformanceStore::kMaxSampleSynthSlots];
        char ref[PerformanceStore::kMaxSampleSynthKitRefLen];
        snprintf(ref, sizeof(ref), "%s/%s", category_folder, filename);
        char zone_dir[64];
        PerformanceStore::SampleSynthKitDir(ref, zone_dir, sizeof(zone_dir));
        int  skipped = 0; // WAVs with no usable note in the name
        bool ok      = PerformanceStore::BuildMultiSampleZones(ref, slots, &skipped);
        if(ok)
        {
            size_t cursor       = 0;
            int    loaded_count = 0;
            int    no_room      = 0;
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
                    no_room++;
                    continue;
                }
                size_t len     = 0;
                bool   file_ok = false;
                for(int attempt = 0; attempt < 3 && !(file_ok && len > 0); attempt++)
                {
                    // Same 3-try retry as a kit load -- see LoadSampleSynthKitByRef().
                    SampleLoadRetryPause(attempt);
                    len     = 0;
                    file_ok = PerformanceStore::ImportWav16(
                        slots[i].filename, sample_synth_arena_l_ + cursor,
                        sample_synth_arena_r_ + cursor, sample_synth_arena_capacity_ - cursor,
                        &len, nullptr, zone_dir);
                }
                if(!file_ok || len == 0)
                {
                    sample_synth_->ClearSlot(i);
                    skipped++;
                    continue;
                }
                if(len >= sample_synth_arena_capacity_ - cursor)
                {
                    // Cut short by the arena's end -- see the kit loader.
                    sample_synth_->ClearSlot(i);
                    no_room++;
                    continue;
                }
                sample_synth_->LoadSlot16(i, sample_synth_arena_l_ + cursor,
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
            if(no_room > 0)
                snprintf(sample_synth_kit_status_, sizeof(sample_synth_kit_status_),
                          "%d zones, %d no room", loaded_count, no_room);
            else if(skipped > 0)
                snprintf(sample_synth_kit_status_, sizeof(sample_synth_kit_status_),
                          "%d zones, %d skipped", loaded_count, skipped);
            else
                snprintf(sample_synth_kit_status_, sizeof(sample_synth_kit_status_), "%d zones",
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
        g_audio_suspended = was_suspended;
        return;
    }

    char wav_folder[40];
    snprintf(wav_folder, sizeof(wav_folder), "%s/%s", PerformanceStore::kSampleSynthFolder,
              category_folder);
    size_t len = 0;
    bool   ok  = PerformanceStore::ImportWav16(filename, sample_synth_arena_l_,
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
        sample_synth_->LoadSlot16(0, sample_synth_arena_l_, sample_synth_arena_r_, (int32_t)len,
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
    g_audio_suspended = was_suspended;
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
    preset.gain01     = sample_synth_->GetGain01();
    preset.vel_sens01 = sample_synth_->GetVelSens01();
    preset.midi_channel = sample_synth_midi_channel_;

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
    ApplySampleSynthVoiceUserPreset(preset);
    sample_synth_voice_user_loaded_slot_ = slot;
    snprintf(sample_synth_voice_user_status_, sizeof(sample_synth_voice_user_status_),
              "Loaded SMPVO%d", slot);
    MarkSampleSynthClean();
}

void Ui::ApplySampleSynthVoiceUserPreset(PerformanceStore::SampleSynthVoiceUserPreset& preset)
{
    // Load the referenced Sound first (sets sample_synth_voice_source_/
    // _sound_name_/_category_ correctly on its own), THEN apply the
    // saved settings on top -- same order Tune's own K2/B2 handlers
    // and SwitchSampleSynthMode()'s own Voice-restore path already use.
    // Saved before Sounds moved into numbered banks: plain "SOUNDS" is
    // what's now SOUNDS1.
    if(strcmp(preset.category, "SOUNDS") == 0)
        snprintf(preset.category, sizeof(preset.category), "SOUNDS1");
    if(preset.filename[0] != '\0')
    {
        sample_synth_capture_unsaved_ = false; // the capture is being replaced
        LoadSampleSynthFileIntoEngine(preset.category, preset.filename);
    }
    sample_synth_->ApplyPreset(preset.params);
    sample_synth_->SetGain01(preset.gain01);
    sample_synth_->SetVelSens01(preset.vel_sens01);
    ApplyPresetMidiChannel(preset.midi_channel, &sample_synth_midi_channel_);
    sample_synth_->SetChromaticMono(preset.mono);
    sample_synth_imp_tune_semis_ = preset.tune_semis;
    sample_synth_imp_mono_       = preset.mono;
    for(int si = 0; si < SampleSynth::kMaxSampleSlots; si++)
        if(sample_synth_->SlotHasSample(si))
            sample_synth_->SetSlotPitchOffsetSemis(si, preset.tune_semis);
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
    preset.gain01             = sample_synth_->GetGain01();
    preset.vel_sens01         = sample_synth_->GetVelSens01();
    preset.midi_channel       = sample_drum_midi_channel_;

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
    if(!ApplySampleSynthDrumUserPreset(preset))
    {
        snprintf(sample_synth_drum_user_status_, sizeof(sample_synth_drum_user_status_),
                  "Kit missing");
        return;
    }
    sample_synth_drum_user_loaded_slot_       = slot;
    snprintf(sample_synth_drum_user_status_, sizeof(sample_synth_drum_user_status_),
              "Loaded SMPDR%d", slot);
    MarkSampleSynthClean();
}

bool Ui::ApplySampleSynthDrumUserPreset(const PerformanceStore::SampleSynthDrumUserPreset& preset)
{
    if(preset.kit_ref[0] != '\0' && !LoadSampleSynthKitByRef(preset.kit_ref))
        return false;
    sample_synth_->ApplyPreset(preset.params);
    sample_synth_->SetGain01(preset.gain01);
    sample_synth_->SetVelSens01(preset.vel_sens01);
    ApplyPresetMidiChannel(preset.midi_channel, &sample_drum_midi_channel_);
    sample_synth_setup_pending_slices_ = preset.break_divide_count;
    sample_synth_setup_tune_semis_     = preset.break_tune_semis;
    ApplySampleSynthBreakDivide(sample_synth_setup_pending_slices_, sample_synth_setup_tune_semis_);
    sample_synth_setup_applied_slices_ = sample_synth_setup_pending_slices_;
    return true;
}

void Ui::RefreshGranularPresetSlots()
{
    granular_preset_user_slot_count_ = PerformanceStore::ListGranularPresets(
        granular_preset_user_slots_, kMaxGranularPresetSlots);
    if(granular_user_cursor_ > granular_preset_user_slot_count_)
        granular_user_cursor_ = granular_preset_user_slot_count_; // index 0 is "New"
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
    preset.midi_channel = granular_midi_channel_;

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
        granular_loaded_preset_slot_   = slot;
        granular_loaded_lib_name_[0]   = '\0'; // it's a user save now
        granular_loaded_lib_folder_[0] = '\0';
        snprintf(granular_preset_status_, sizeof(granular_preset_status_), "Saved GRAINS%d", slot);
        granular_preset_slots_dirty_ = true; // a new slot may now exist
        MarkGranularClean();
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
    granular_loaded_preset_slot_   = 0;
    granular_loaded_lib_name_[0]   = '\0';
    granular_loaded_lib_folder_[0] = '\0';
    snprintf(granular_preset_status_, sizeof(granular_preset_status_), "New");
    MarkGranularClean();
}

void Ui::LoadGranularUserSlot(int slot)
{
    if(!granular_)
        return;
    GranularEngine::GranularPresetData preset;
    size_t audio_len = 0;
    // Full g_audio_suspended, not just detaching the engine -- real
    // hardware showed the same audio-ISR-vs-SDMMC-DMA contention every
    // other load/save here guards against (AUD0:1, FR_DISK_ERR otherwise).
    // Loaded audio lands in the same owned capture buffer Direct Record/
    // From-Layer already use.
    g_audio_suspended = true;
    g_progress_disp   = disp_;
    bool ok = PerformanceStore::LoadGranularPreset(
        slot, &preset, granular_capture_buf_l_, granular_capture_buf_r_,
        granular_capture_capacity_, &audio_len, &Ui::OnSaveLoadProgress);
    g_progress_disp   = nullptr;
    g_audio_suspended = false;

    if(ok)
    {
        granular_->ApplyPreset(preset);
        ApplyPresetMidiChannel(preset.midi_channel, &granular_midi_channel_);
        granular_->SetSource(granular_capture_buf_l_, granular_capture_buf_r_, audio_len);
        OnNewGranularCapture(audio_len);
        granular_loaded_preset_slot_   = slot;
        granular_loaded_lib_name_[0]   = '\0';
        granular_loaded_lib_folder_[0] = '\0';
        snprintf(granular_preset_status_, sizeof(granular_preset_status_), "Loaded GRAINS%d", slot);
        MarkGranularClean();
    }
    else
    {
        snprintf(granular_preset_status_, sizeof(granular_preset_status_), "Fail:%s",
                  PerformanceStore::GetLastError());
    }
}

void Ui::GranularLibFolder(int idx, char* name, size_t name_size, char* label,
                           size_t label_size) const
{
    if(idx <= 0 || idx > granular_captured_bank_count_)
    {
        snprintf(name, name_size, "%s", PerformanceStore::kGranularStarterFolder);
        snprintf(label, label_size, "Starter");
        return;
    }
    int n = granular_captured_banks_[idx - 1];
    snprintf(name, name_size, "CAPTURED%d", n);
    snprintf(label, label_size, "Captured %d", n);
}

void Ui::GranularLibDisplayName(const char* file, char* out, size_t out_size)
{
    snprintf(out, out_size, "%s", file);
    size_t len = strlen(out);
    if(len > 4 && strcasecmp(out + len - 4, ".dat") == 0)
        out[len - 4] = '\0';
}

void Ui::LoadGranularLibraryItem()
{
    if(!granular_ || granular_lib_folder_ < 0 || granular_lib_cursor_ >= granular_lib_count_)
        return;
    char folder[16], label[24];
    GranularLibFolder(granular_lib_folder_, folder, sizeof(folder), label, sizeof(label));
    const char* name = granular_lib_names_[granular_lib_cursor_];

    GranularEngine::GranularPresetData preset;
    size_t audio_len = 0;
    g_audio_suspended = true; // see LoadGranularUserSlot()
    g_progress_disp   = disp_;
    bool ok = PerformanceStore::LoadGranularLibraryPreset(
        folder, name, &preset, granular_capture_buf_l_, granular_capture_buf_r_,
        granular_capture_capacity_, &audio_len, &Ui::OnSaveLoadProgress);
    g_progress_disp   = nullptr;
    g_audio_suspended = false;

    if(ok)
    {
        granular_->ApplyPreset(preset);
        ApplyPresetMidiChannel(preset.midi_channel, &granular_midi_channel_);
        granular_->SetSource(granular_capture_buf_l_, granular_capture_buf_r_, audio_len);
        OnNewGranularCapture(audio_len);
        // Not a user save -- the User page's Save starts as Save New.
        granular_loaded_preset_slot_ = 0;
        snprintf(granular_loaded_lib_folder_, sizeof(granular_loaded_lib_folder_), "%s", folder);
        snprintf(granular_loaded_lib_name_, sizeof(granular_loaded_lib_name_), "%s", name);
        char shown[61];
        GranularLibDisplayName(name, shown, sizeof(shown));
        snprintf(granular_preset_status_, sizeof(granular_preset_status_), "Loaded %s", shown);
        MarkGranularClean();
    }
    else
    {
        snprintf(granular_preset_status_, sizeof(granular_preset_status_), "Fail:%s",
                  PerformanceStore::GetLastError());
    }
}

void Ui::TriggerSaveGranularCapture()
{
    if(!granular_)
        return;
    if(granular_capture_full_len_ == 0 || granular_->GetSourceLen() == 0)
    {
        snprintf(granular_trim_status_, sizeof(granular_trim_status_), "Nothing captured");
        return;
    }
    if(!PerformanceStore::IsCardPresent())
    {
        PerformanceStore::Remount();
        if(!PerformanceStore::IsCardPresent())
        {
            snprintf(granular_trim_status_, sizeof(granular_trim_status_), "No card");
            return;
        }
    }
    GranularEngine::GranularPresetData preset = granular_->CapturePreset();
    char folder[16], name[16];
    g_audio_suspended = true; // see TriggerSaveGranularPreset()
    g_progress_disp   = disp_;
    bool ok = PerformanceStore::SaveGranularCapture(
        preset, granular_->GetSourceL(), granular_->GetSourceR(), granular_->GetSourceLen(),
        folder, sizeof(folder), name, sizeof(name), &Ui::OnSaveLoadProgress);
    g_progress_disp   = nullptr;
    g_audio_suspended = false;

    if(ok)
    {
        granular_loaded_preset_slot_ = 0;
        snprintf(granular_loaded_lib_folder_, sizeof(granular_loaded_lib_folder_), "%s", folder);
        snprintf(granular_loaded_lib_name_, sizeof(granular_loaded_lib_name_), "%s", name);
        char shown[16];
        GranularLibDisplayName(name, shown, sizeof(shown));
        snprintf(granular_trim_status_, sizeof(granular_trim_status_), "Saved %s/%s", folder,
                  shown);
        granular_lib_dirty_       = true;
        granular_lib_items_dirty_ = true;
        MarkGranularClean();
    }
    else
    {
        snprintf(granular_trim_status_, sizeof(granular_trim_status_), "Fail:%s",
                  PerformanceStore::GetLastError());
    }
}

Ui::EditSnapshot Ui::GranularSnapshot() const
{
    EditSnapshot s;
    if(!granular_)
        return s;
    GranularEngine::GranularPresetData p = granular_->CapturePreset();
    s.Add(p.size01);
    s.Add(p.fill01);
    s.Add(p.scan_fill01);
    s.Add(p.gap01);
    s.Add(p.scan_gap01);
    s.Add(p.position01);
    s.Add(p.scan01);
    s.Add(p.scan_start01);
    s.Add(p.scan_end01);
    s.Add(p.grain_tune01);
    s.Add(p.grain_follows_note);
    s.Add(p.direction01);
    s.Add(p.scan_direction);
    s.Add(p.attack01);
    s.Add(p.decay01);
    s.Add(p.sustain01);
    s.Add(p.release01);
    s.Add(p.filter_mode);
    s.Add(p.filter_cutoff01);
    s.Add(p.filter_res01);
    s.Add(p.grain_volume01);
    s.Add(p.scan_volume01);
    s.Add(p.rhythm_index);
    s.Add(p.grain_speed);
    s.Add(p.jitter01);
    s.Add(p.scan_bounce);
    s.Add(p.reverb_send01);
    s.Add(p.output_level01);
    s.Add(p.pan01);
    s.Add(p.gain01);
    s.Add(p.delay_send01);
    s.Add(granular_midi_channel_);
    s.Add(granular_trim_start01_);
    s.Add(granular_trim_end01_);
    return s;
}

void Ui::MarkGranularClean()
{
    granular_baseline_        = GranularSnapshot();
    granular_capture_unsaved_ = false;
}

bool Ui::GranularUnsaved() const
{
    return granular_capture_unsaved_
           || (granular_baseline_.len > 0 && !(GranularSnapshot() == granular_baseline_));
}

void Ui::RefreshDexedPresetSlots()
{
    dexed_import_folder_count_ = PerformanceStore::ListDexedImportFolders(
        dexed_import_folders_, PerformanceStore::kMaxDexedImportFolders);

    // Full unfiltered scan, then drop anything that belongs to a
    // recorded import folder -- what's left is the User group's own
    // list (see dexed_preset_user_slots_'s own doc comment). Needs the
    // import-folder refresh above to have already happened this call.
    // Every user-range slot on the card, imports included -- capped only by
    // the slot range itself, never by the (smaller) User list's size: with
    // 8 imported banks the old 200 cap filled up with import presets before
    // ever reaching the user saves after them, hiding new saves.
    static int DSY_D2_BSS raw_slots[kMaxDexedRawSlots]; // UI scratch -- see itcm.h
    int        raw_count = PerformanceStore::ListDexedPresets(raw_slots, kMaxDexedRawSlots);
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
        case DexedFilesGroup::Imports:
        default: folder_count = dexed_import_folder_count_; break;
    }
    if(dexed_preset_cursor_ >= folder_count)
        dexed_preset_cursor_ = folder_count > 0 ? folder_count - 1 : 0;
    if(dexed_user_cursor_ > dexed_preset_user_slot_count_)
        dexed_user_cursor_ = dexed_preset_user_slot_count_; // index 0 is "New"
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
        default:
        {
            if(dexed_preset_folder_cursor_ < 0
               || dexed_preset_folder_cursor_ >= dexed_import_folder_count_)
                return -1;
            const PerformanceStore::DexedImportFolder& f
                = dexed_import_folders_[dexed_preset_folder_cursor_];
            int slot = f.first_slot + dexed_preset_cursor_;
            return slot <= f.last_slot ? slot : -1;
        }
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
    DexedSynth::DexedPresetData preset = dexed_->CapturePreset();
    preset.midi_channel                = DexedActiveChannel();
    // See the g_audio_suspended comment in TriggerSave() above. Dexed
    // presets are a single small struct (no chunked audio), so the write
    // is much shorter-lived than Performance/Grains, but it's still a
    // blocking SD write and the same race is possible in principle. The
    // name check reads every User save's name, so it sits inside too.
    g_audio_suspended = true;
    bool renamed      = NumberDexedSaveName(&preset, slot);
    bool ok           = PerformanceStore::SaveDexedPreset(slot, preset);
    g_audio_suspended = false;
    if(ok && renamed)
    {
        // The live patch takes the new name too, so it matches the save.
        for(int i = 0; i < 10; i++)
            dexed_->SetPatchByte(145 + i, preset.patch[145 + i]);
    }
    if(ok)
    {
        dexed_loaded_preset_slot_ = slot;
        dexed_browse_name_slot_   = -2; // its name may have changed
        if(renamed)
            snprintf(dexed_preset_status_, sizeof(dexed_preset_status_), "Saved as %s",
                      DexedSynth::GetPresetDataName(preset));
        else
            snprintf(dexed_preset_status_, sizeof(dexed_preset_status_), "Saved DEXED%d", slot);
        dexed_preset_slots_dirty_ = true; // a new slot may now exist
        MarkDexedClean();
    }
    else
    {
        snprintf(dexed_preset_status_, sizeof(dexed_preset_status_), "Fail:%s",
                  PerformanceStore::GetLastError());
    }
}

bool Ui::NumberDexedSaveName(DexedSynth::DexedPresetData* p, int target_slot)
{
    // Every other User save's name (not factory or imported voices --
    // those live in their own groups). static: up to 200 x 11 bytes.
    static char DSY_D2_BSS names[kMaxDexedPresetSlots][11];
    if(dexed_preset_slots_dirty_)
        RefreshDexedPresetSlots();
    int n_names = 0;
    for(int i = 0; i < dexed_preset_user_slot_count_; i++)
    {
        if(dexed_preset_user_slots_[i] == target_slot)
            continue; // overwriting this one -- its old name doesn't count
        DexedSynth::DexedPresetData other;
        if(PerformanceStore::LoadDexedPreset(dexed_preset_user_slots_[i], &other))
            snprintf(names[n_names++], 11, "%s", DexedSynth::GetPresetDataName(other));
    }
    auto taken = [&](const char* name) {
        for(int i = 0; i < n_names; i++)
            if(strcmp(names[i], name) == 0)
                return true;
        return false;
    };

    char name[11];
    snprintf(name, sizeof(name), "%s", DexedSynth::GetPresetDataName(*p));
    if(!taken(name))
        return false;

    // Saving again from an already-numbered copy ("BASS 2", where "BASS"
    // is also a save) numbers from the base name, not "BASS 2 2".
    char   base[11];
    snprintf(base, sizeof(base), "%s", name);
    size_t len = strlen(base);
    size_t d   = len;
    while(d > 0 && base[d - 1] >= '0' && base[d - 1] <= '9')
        d--;
    if(d > 1 && d < len && base[d - 1] == ' ')
    {
        char stripped[11];
        snprintf(stripped, sizeof(stripped), "%.*s", (int)(d - 1), base);
        if(taken(stripped))
            snprintf(base, sizeof(base), "%s", stripped);
    }

    // "NAME n", with NAME cut short to fit DX7's 10 characters.
    char candidate[11];
    for(int num = 2; num < 1000; num++)
    {
        char suffix[6];
        snprintf(suffix, sizeof(suffix), " %d", num);
        int keep = 10 - (int)strlen(suffix);
        int blen = (int)strlen(base);
        if(blen > keep)
            blen = keep;
        while(blen > 0 && base[blen - 1] == ' ')
            blen--;
        snprintf(candidate, sizeof(candidate), "%.*s%s", blen, base, suffix);
        if(!taken(candidate))
            break;
    }
    // Space-padded, the DX7 patch's own name format.
    size_t clen = strlen(candidate);
    for(int i = 0; i < 10; i++)
        p->patch[145 + i] = (uint8_t)(i < (int)clen ? candidate[i] : ' ');
    return true;
}

void Ui::SelectDexedEngine(int engine)
{
    engine = engine == 1 ? 1 : 0;
    if(engine == dexed_active_ || !dexed_engines_[engine])
        return;
    // Swap the per-engine state: what's loaded and its unsaved baseline.
    int          slot     = dexed_loaded_preset_slot_;
    EditSnapshot baseline = dexed_baseline_;
    dexed_loaded_preset_slot_ = dexed_parked_slot_;
    dexed_baseline_           = dexed_parked_baseline_;
    dexed_parked_slot_        = slot;
    dexed_parked_baseline_    = baseline;

    dexed_active_          = engine;
    dexed_                 = dexed_engines_[engine];
    dexed_browse_name_slot_ = -2; // the loaded slot's name comes from the live patch
    dexed_preset_status_[0] = '\0';
}

void Ui::SelectEnsembleEngine(int engine)
{
    engine = engine == 1 ? 1 : 0;
    if(engine == ensemble_active_ || !ensemble_engines_[engine])
        return;
    // Same swap as SelectDexedEngine(): what's loaded and its baseline.
    int          slot     = ensemble_loaded_preset_slot_;
    EditSnapshot baseline = ensemble_baseline_;
    ensemble_loaded_preset_slot_ = ensemble_parked_slot_;
    ensemble_baseline_           = ensemble_parked_baseline_;
    ensemble_parked_slot_        = slot;
    ensemble_parked_baseline_    = baseline;

    ensemble_active_ = engine;
    ensemble_        = ensemble_engines_[engine];
    // Each keeps its User saves in its own folder (ENSEMBLE/, ORGAN/).
    PerformanceStore::SetEnsemblePresetFolder(engine == 1);
    ensemble_preset_slots_dirty_ = true;
    ensemble_preset_status_[0]   = '\0';
}

int Ui::DexedVoiceRoom(int engine) const
{
    bool other_on = engine == 0 ? dexed_b_enabled_ : dexed_enabled_;
    if(!other_on || !dexed_engines_[1 - engine])
        return DexedSynth::kMaxVoices;
    int room = kDexedPairVoices - dexed_engines_[1 - engine]->GetMaxVoices();
    return room < DexedSynth::kMaxVoices ? room : DexedSynth::kMaxVoices;
}

void Ui::ApplyDexedVoiceCeiling(int keep)
{
    if(!(dexed_enabled_ && dexed_b_enabled_) || !dexed_engines_[0] || !dexed_engines_[1])
        return;
    DexedSynth* other = dexed_engines_[1 - keep];
    int         room  = kDexedPairVoices - dexed_engines_[keep]->GetMaxVoices();
    if(other->GetMaxVoices() > room)
        other->SetMaxVoices(room);
}

void Ui::ResyncKnobs()
{
    KnobContext ctx       = CurrentKnobContext();
    size_t      ctx_index = (size_t)ctx;
    k1_pickup_engaged_[ctx_index] = false;
    k2_pickup_engaged_[ctx_index] = false;
    SyncPickupTargets(ctx);
    last_knob_context_ = ctx;
}

void Ui::FormatPan(float pan01, char* out, size_t out_size)
{
    // Same 0..100% (50% = centre) as the other pan readouts.
    snprintf(out, out_size, "%d%%", (int)(pan01 * 100.f + 0.5f));
}

void Ui::DrawDexedMixColumn(int cx, const char* name, bool selected, float level01, float pan01)
{
    // Name (inverted when it's the one the knobs set), the level as a
    // bar, and the pan as a slider underneath.
    int nw = TomThumbAdvanceWidth(name);
    // 13 px wide so the bar, the label and the pan slider all centre on
    // the same column, cx.
    if(selected)
        disp_->DrawRect(cx - nw / 2 - 2, 11, cx - nw / 2 + nw, 18, true, true);
    TomThumbDrawText(disp_, cx - nw / 2, 17, name, !selected);
    DrawMixerVBar(cx - 6, 20, 13, 17, level01, false);
    disp_->DrawLine(cx - 9, 41, cx + 9, 41, true);
    disp_->DrawPixel(cx, 40, true);
    disp_->DrawPixel(cx, 42, true);
    int px = cx - 9 + (int)lroundf(pan01 * 18.f);
    disp_->DrawRect(px - 1, 39, px + 1, 43, true, true);
}

void Ui::TriggerNewDexedPreset()
{
    if(!dexed_)
        return;
    // INIT VOICE, with the Pod's own sound extras back to neutral
    // (Filter open, Fold off, wheel on Pitch -- Brightness/Env Speed
    // re-centre inside SetPatch()). Mix (Level/Pan/Gain/sends) and the
    // MIDI channel are where the sound sits in your setup, not the
    // sound itself, so they stay as they are.
    DexedSynth::DexedPresetData cur  = dexed_->CapturePreset();
    DexedSynth::DexedPresetData init = DexedSynth::GetInitVoice();
    init.reverb_send01    = cur.reverb_send01;
    init.output_level01   = cur.output_level01;
    init.pan01            = cur.pan01;
    init.delay_send01     = cur.delay_send01;
    init.gain01           = cur.gain01;
    init.mod_wheel_target = (int32_t)DexedSynth::ModWheelTarget::Pitch;
    dexed_->ApplyPreset(init, DexedApplyBus());
    dexed_loaded_preset_slot_ = 0; // not a stored slot -- shown by its patch name
    snprintf(dexed_preset_status_, sizeof(dexed_preset_status_), "New (INIT VOICE)");
    MarkDexedClean();
}

void Ui::LoadDexedSlot(int slot)
{
    if(!dexed_ || slot < 1)
        return;
    DexedSynth::DexedPresetData preset;
    bool ok = PerformanceStore::LoadDexedPreset(slot, &preset);
    if(ok)
    {
        dexed_->ApplyPreset(preset, DexedApplyBus());
        ApplyPresetMidiChannel(preset.midi_channel, DexedActiveChannelPtr());
        dexed_loaded_preset_slot_ = slot;
        snprintf(dexed_preset_status_, sizeof(dexed_preset_status_), "Loaded %s",
                  DexedSlotName(slot));
        MarkDexedClean();
    }
    else
    {
        snprintf(dexed_preset_status_, sizeof(dexed_preset_status_), "Fail:%s",
                  PerformanceStore::GetLastError());
    }
}

const char* Ui::DexedSlotName(int slot)
{
    if(slot <= 0)
    {
        // Nothing stored is loaded (New) -- name it by the live patch's
        // own name bytes, e.g. INIT VOICE.
        if(!dexed_)
            return "(custom)";
        static char live[11];
        for(int i = 0; i < 10; i++)
            live[i] = (char)dexed_->GetPatchByte(145 + i);
        live[10] = '\0';
        for(int i = 9; i >= 0 && live[i] == ' '; i--)
            live[i] = '\0';
        return live[0] ? live : "(custom)";
    }
    if(slot <= DexedSynth::GetNumFactoryPresets())
        return DexedSynth::GetFactoryPresetName(slot - 1);
    // A user slot's name lives inside its own saved file -- read it once
    // per distinct slot, not once per Draw() tick (see
    // dexed_browse_name_slot_'s own comment). The loaded slot is already
    // live in RAM, so no SD read for that one.
    if(slot != dexed_browse_name_slot_)
    {
        const char* name = "?";
        DexedSynth::DexedPresetData preset;
        if(slot == dexed_loaded_preset_slot_ && dexed_)
            name = DexedSynth::GetPresetDataName(dexed_->CapturePreset());
        else if(PerformanceStore::LoadDexedPreset(slot, &preset))
            name = DexedSynth::GetPresetDataName(preset);
        // Older saves can carry blank name bytes -- show "DEXEDn" (the
        // Global:SdMgmt short code) rather than a bare "?".
        if(strcmp(name, "?") == 0)
            snprintf(dexed_browse_name_buf_, sizeof(dexed_browse_name_buf_), "DEXED%d", slot);
        else
            snprintf(dexed_browse_name_buf_, sizeof(dexed_browse_name_buf_), "%s", name);
        dexed_browse_name_slot_ = slot;
    }
    return dexed_browse_name_buf_;
}

Ui::EditSnapshot Ui::DexedSnapshot() const
{
    EditSnapshot s;
    if(!dexed_)
        return s;
    DexedSynth::DexedPresetData p = dexed_->CapturePreset();
    for(size_t i = 0; i < sizeof(p.patch); i++)
        s.Add(p.patch[i]);
    s.Add(p.reverb_send01);
    s.Add(p.output_level01);
    s.Add(p.filter_mode);
    s.Add(p.filter_cutoff01);
    s.Add(p.filter_res01);
    s.Add(p.pan01);
    s.Add(p.delay_send01);
    s.Add(p.fold_amount01);
    s.Add(p.fold_trim01);
    s.Add(p.gain01);
    s.Add(p.mod_wheel_target);
    s.Add(p.op_switch);
    s.Add(DexedActiveChannel());
    return s;
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
    // D2 SRAM is fine: ImportDexedSyx() decodes into it from its own
    // buffer and SaveDexedPreset() copies out before writing, so the SD
    // DMA never touches it (see DSY_D2_BSS in itcm.h).
    static DexedSynth::DexedPresetData DSY_D2_BSS presets[PerformanceStore::kMaxSyxBulkVoices];
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
    dexed_browse_name_slot_   = -2;
    MarkDexedClean();

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

    // Imported -- file it away in DEXED IMPORT/Done, same as the Sampler's
    // pack imports, so the Import list only shows what's still to do.
    PerformanceStore::MoveToImportDoneFolder("DEXED IMPORT",
                                             dexed_import_names_[dexed_import_cursor_]);
    dexed_import_files_dirty_ = true;

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
    int total = ensemble_->GetNumFactoryPresets() + ensemble_preset_user_slot_count_;
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
    int slot = (!force_new && EnsembleIsUserSlot(ensemble_loaded_preset_slot_))
                   ? ensemble_loaded_preset_slot_
                   : PerformanceStore::NextFreeEnsemblePresetSlot();
    if(slot < 0)
    {
        snprintf(ensemble_preset_status_, sizeof(ensemble_preset_status_), "Card full/missing");
        return;
    }
    EnsembleSynth::EnsemblePresetData preset = ensemble_->CapturePreset();
    preset.midi_channel                      = *EnsembleActiveChannelPtr();
    // See the g_audio_suspended comment in TriggerSave() above -- same
    // short-lived single-struct write as TriggerSaveDexedPreset(), same
    // in-principle race.
    g_audio_suspended = true;
    bool ok           = PerformanceStore::SaveEnsemblePreset(slot, preset);
    g_audio_suspended = false;
    if(ok)
    {
        ensemble_loaded_preset_slot_ = slot;
        snprintf(ensemble_preset_status_, sizeof(ensemble_preset_status_), "Saved %d", slot);
        MarkEnsembleClean();
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
    ensemble_->ApplyPreset(ensemble_->GetNewPreset());
    ensemble_loaded_preset_slot_ = 0;
    snprintf(ensemble_preset_status_, sizeof(ensemble_preset_status_), "New");
    MarkEnsembleClean();
}

void Ui::LoadEnsembleSlot(int slot)
{
    if(!ensemble_ || slot < 1)
        return;
    // Factory presets aren't files -- straight from the firmware table.
    bool factory = EnsembleIsFactorySlot(slot);
    EnsembleSynth::EnsemblePresetData preset;
    bool ok = factory ? true : PerformanceStore::LoadEnsemblePreset(slot, &preset);
    if(factory)
        preset = ensemble_->GetFactoryPreset(slot - kEnsembleFactorySlotBase);
    if(ok)
    {
        ensemble_->ApplyPreset(preset);
        ApplyPresetMidiChannel(preset.midi_channel, EnsembleActiveChannelPtr());
        ensemble_loaded_preset_slot_ = slot;
        if(factory)
            snprintf(ensemble_preset_status_, sizeof(ensemble_preset_status_), "Loaded %s",
                      ensemble_->GetFactoryPresetName(slot - kEnsembleFactorySlotBase));
        else
            snprintf(ensemble_preset_status_, sizeof(ensemble_preset_status_), "Loaded %s%d",
                      EnsembleSlotPrefix(), slot);
        MarkEnsembleClean();
    }
    else
    {
        snprintf(ensemble_preset_status_, sizeof(ensemble_preset_status_), "Fail:%s",
                  PerformanceStore::GetLastError());
    }
}

Ui::EditSnapshot Ui::EnsembleSnapshotFor(int engine) const
{
    EditSnapshot         s;
    const EnsembleSynth* e = ensemble_engines_[engine == 1 ? 1 : 0];
    if(!e)
        return s;
    EnsembleSynth::EnsemblePresetData p = e->CapturePreset();
    for(int i = 0; i < EnsembleSynth::kNumDrawbars; i++)
        s.Add(p.drawbar_levels[i]);
    s.Add(p.chorus_depth01);
    s.Add(p.chorus_rate01);
    s.Add(p.attack01);
    s.Add(p.decay01);
    s.Add(p.sustain01);
    s.Add(p.release01);
    s.Add(p.filter_mode);
    s.Add(p.filter_cutoff01);
    s.Add(p.filter_res01);
    s.Add(p.output_level01);
    s.Add(p.pan01);
    s.Add(p.reverb_send01);
    s.Add(p.delay_send01);
    s.Add(p.gain01);
    s.Add(p.mod_wheel_target);
    s.Add(p.max_voices);
    s.Add(p.organ_low_drawbars[0]);
    s.Add(p.organ_low_drawbars[1]);
    s.Add(p.perc_on);
    s.Add(p.perc_third);
    s.Add(p.perc_decay01);
    s.Add(p.perc_level01);
    s.Add(p.click01);
    s.Add(p.vel_sens01);
    s.Add(p.lfo_wave);
    s.Add(p.lfo_rate01);
    s.Add(p.lfo_sync);
    s.Add(p.lfo_sync_index);
    s.Add(p.lfo_fade01);
    s.Add(p.lfo_depth01);
    s.Add(engine == 1 ? organ_midi_channel_ : ensemble_midi_channel_);
    return s;
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
                                            // MIDI channels aren't startup defaults
                                            // any more -- PREFS.DAT keeps the fields
                                            // (its header is the performance one),
                                            // written as Ch1 and never read back.
                                            0, 0, 0, 0, GetDelayState());
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
                                            GetDelaySettings(), delay_to_reverb01_,
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
                                            GetDelaySettings(), delay_to_reverb01_,
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

void Ui::DrawDexedSplitPage()
{
    // Keyboard C1 (24) .. F5 (77): 32 white keys of 4 px across the full
    // width, with the zones A and B play drawn above it.
    constexpr int kKeyY0 = 33, kKeyY1 = 45, kKeyW = 4, kFirstNote = 24;
    static const int kWhiteOffsets[7] = {0, 2, 4, 5, 7, 9, 11};
    disp_->DrawRect(0, kKeyY0, disp_->Width() - 1, kKeyY1, true, false);
    for(int k = 1; k < 32; k++)
        disp_->DrawLine(k * kKeyW, kKeyY0, k * kKeyW, kKeyY1, true);
    for(int k = 0; k < 32; k++)
    {
        int n = k % 7;
        if(n == 2 || n == 6)
            continue; // no black key after E or B
        int x = k * kKeyW + kKeyW - 1;
        disp_->DrawRect(x, kKeyY0, x + 2, kKeyY0 + 8, true, true);
    }

    // Zone: a span with end stops and its letter on a small plate.
    auto zone = [&](int x0, int x1, int y, const char* label) {
        disp_->DrawLine(x0, y, x1, y, true);
        disp_->DrawLine(x0, y - 2, x0, y + 2, true);
        disp_->DrawLine(x1, y - 2, x1, y + 2, true);
        // A 7x7 square plate: the 3x5 letter (rows y-3..y+1) with two
        // clear pixels either side and one above and below.
        int tx = (x0 + x1) / 2 - TomThumbAdvanceWidth(label) / 2;
        int iw = TomThumbInkWidth(label);
        disp_->DrawRect(tx - 2, y - 4, tx + iw + 1, y + 2, true, true);
        TomThumbDrawText(disp_, tx, y + 2, label, false);
    };

    char oa[8], ob[8], line[24];
    snprintf(oa, sizeof(oa), "A %+d", dexed_octave_[0]);
    snprintf(ob, sizeof(ob), "B %+d", dexed_octave_[1]);
    snprintf(line, sizeof(line), "%s   %s", oa, ob);
    TomThumbDrawText(disp_, 1, 16, dexed_split_ ? "SPLIT" : "LAYER", true);
    TomThumbDrawText(disp_, disp_->Width() - 1 - TomThumbAdvanceWidth(line), 16, line, true);

    char point[8];
    Dx7NoteName(dexed_split_note_, point, sizeof(point));
    if(dexed_split_)
    {
        // The split note's left edge: a white key's own, or for a black
        // key the line between the two whites it sits on.
        int rel = dexed_split_note_ - kFirstNote, oct = rel / 12, n = rel % 12, wi = 0;
        for(int k = 0; k < 7; k++)
            if(kWhiteOffsets[k] <= n)
                wi = k;
        bool black = kWhiteOffsets[wi] != n;
        int  sx    = (oct * 7 + wi + (black ? 1 : 0)) * kKeyW;
        zone(0, sx - 2, 25, "A");
        zone(sx + 2, disp_->Width() - 1, 25, "B"); // one clear pixel each side of the marker
        disp_->DrawLine(sx, 19, sx, kKeyY1, true);
        disp_->DrawLine(sx - 1, kKeyY0 - 2, sx + 1, kKeyY0 - 2, true);
    }
    else
    {
        zone(0, disp_->Width() - 1, 21, "A");
        zone(0, disp_->Width() - 1, 29, "B");
    }

    char oct_val[8];
    snprintf(oct_val, sizeof(oct_val), "%+d", dexed_octave_[dexed_split_oct_b_ ? 1 : 0]);
    DrawControlRow(kFooterRow1Y, false, kFooterDividerY, "Point", dexed_split_ ? point : "--",
                     oct_val, dexed_split_oct_b_ ? "B Oct" : "A Oct");
    DrawControlRow(kFooterRow2Y, true, kFooterInterRowDividerY,
                     dexed_split_ ? "Layer Split*" : "Layer* Split", "", "",
                     dexed_split_oct_b_ ? "A B*" : "A* B");
}
