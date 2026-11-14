// Screen::Supersaw -- the Supersaw engine's pages (see SupersawParamPage in
// ui.h and SupersawSynth for what each control does). The hooks into the
// shared screen, knob and button code are in ui.cpp, as for every engine.
#include "ui.h"
#include "audio_engine.h"
#include "performance_store.h"
#include "supersaw_synth.h"
#include <cstdio>
#include <cstring>
#include <math.h>

using namespace daisy;

namespace
{
inline float Clamp(float v, float lo, float hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

// "45%".
void Pct(float v01, char* out, size_t n)
{
    snprintf(out, n, "%d%%", (int)(v01 * 100.f + 0.5f));
}

// A bipolar knob (0.5 = none) as "+45%" / "-20%" / "0%".
void BiPct(float v01, char* out, size_t n)
{
    const int p = (int)((v01 * 2.f - 1.f) * 100.f + (v01 >= 0.5f ? 0.5f : -0.5f));
    if(p == 0)
        snprintf(out, n, "0%%");
    else
        snprintf(out, n, "%+d%%", p);
}

// Seconds, whole numbers only (this printf has no float support).
void Secs(float s, char* out, size_t n)
{
    const int cs = (int)(s * 100.f + 0.5f);
    snprintf(out, n, "%d.%02ds", cs / 100, cs % 100);
}

// Gain01 is 0 to +12 dB, as every engine's (see ui.cpp's FormatGainDb()).
void GainDb(float gain01, char* out, size_t n)
{
    const int tenths = (int)(gain01 * 120.4f + 0.5f);
    snprintf(out, n, "+%d.%ddB", tenths / 10, tenths % 10);
}

const char* const kOsc2Waves[] = {"Saw", "Square", "Tri"};
const char* const kFilterTypes[] = {"LPF", "HPF", "BPF"};
const char* const kVoiceModes[]  = {"Poly", "Mono", "Legato"};

} // namespace

void Ui::SetSupersaw(SupersawSynth* s)
{
    supersaw_ = s;
    MarkSupersawClean();
}

// --- Pages and knobs ---------------------------------------------------

void Ui::SupersawRotatePage(int inc)
{
    const int n = (int)SupersawParamPage::kCount;
    const int p = (((int)supersaw_page_ + inc) % n + n) % n;
    const SupersawParamPage np = (SupersawParamPage)p;
    if((np == SupersawParamPage::Preset || np == SupersawParamPage::User) && np != supersaw_page_)
        ssw_slots_dirty_ = true; // re-scan the card on entry
    supersaw_page_  = np;
    save_load_mode_ = SaveLoadMode::Idle;
}

Ui::KnobContext Ui::SupersawKnobContext() const
{
    switch(supersaw_page_)
    {
        case SupersawParamPage::SuperSaw: return KnobContext::SupersawSuperSaw;
        case SupersawParamPage::Osc2: return KnobContext::SupersawOsc2;
        case SupersawParamPage::Balance: return KnobContext::SupersawBalance;
        case SupersawParamPage::Filter: return KnobContext::SupersawFilter;
        case SupersawParamPage::FltEnv:
            return ssw_fenv_sr_ ? KnobContext::SupersawFltEnvSR : KnobContext::SupersawFltEnvAD;
        case SupersawParamPage::FltMod: return KnobContext::SupersawFltMod;
        case SupersawParamPage::AmpEnv:
            return ssw_aenv_sr_ ? KnobContext::SupersawAmpEnvSR : KnobContext::SupersawAmpEnvAD;
        case SupersawParamPage::Lfo: return KnobContext::SupersawLfo;
        case SupersawParamPage::LfoDepth: return KnobContext::SupersawLfoDepth;
        case SupersawParamPage::Voice: return KnobContext::SupersawVoice;
        case SupersawParamPage::Chorus: return KnobContext::SupersawChorus;
        case SupersawParamPage::Mix:
            return KnobContext::SupersawMix;
        case SupersawParamPage::FX:
            return ssw_fx_delay_ ? KnobContext::SupersawFxDelay : KnobContext::SupersawFxReverb;
        case SupersawParamPage::MIDI:
            return ssw_midi_vel_ ? KnobContext::SupersawMidiVel : KnobContext::SupersawMidi;
        default: return KnobContext::SupersawList;
    }
}

void Ui::SupersawSyncPickup(KnobContext ctx, size_t i)
{
    SupersawSynth* s = supersaw_;
    if(!s)
        return;
    using E = SupersawSynth;
    switch(ctx)
    {
        case KnobContext::SupersawSuperSaw:
            k1_pickup_raw_[i] = s->GetDetune01();
            k2_pickup_raw_[i] = s->GetMix01();
            break;
        case KnobContext::SupersawOsc2:
            k1_pickup_raw_[i] = ((float)(s->GetOsc2Coarse() + 24) + 0.5f) / 48.99f;
            k2_pickup_raw_[i] = (s->GetOsc2Fine() + 50.f) / 100.f;
            break;
        case KnobContext::SupersawBalance: k1_pickup_raw_[i] = s->GetBalance01(); break;
        case KnobContext::SupersawFilter:
            k1_pickup_raw_[i] = s->GetCutoff01();
            k2_pickup_raw_[i] = s->GetResonance01();
            break;
        case KnobContext::SupersawFltEnvAD:
        case KnobContext::SupersawAmpEnvAD:
        {
            const int e = ctx == KnobContext::SupersawFltEnvAD ? E::kFilterEnv : E::kAmpEnv;
            k1_pickup_raw_[i] = s->GetAttack01(e);
            k2_pickup_raw_[i] = s->GetDecay01(e);
            break;
        }
        case KnobContext::SupersawFltEnvSR:
        case KnobContext::SupersawAmpEnvSR:
        {
            const int e = ctx == KnobContext::SupersawFltEnvSR ? E::kFilterEnv : E::kAmpEnv;
            k1_pickup_raw_[i] = s->GetSustain01(e);
            k2_pickup_raw_[i] = s->GetRelease01(e);
            break;
        }
        case KnobContext::SupersawFltMod:
            k1_pickup_raw_[i] = s->GetFilterEnvDepth01();
            k2_pickup_raw_[i] = s->GetKeyFollow01();
            break;
        case KnobContext::SupersawLfo: LfoSyncPickup(s->Lfo(), false, 0, i); break;
        case KnobContext::SupersawLfoDepth: LfoSyncPickup(s->Lfo(), true, ssw_lfo_dest_, i); break;
        case KnobContext::SupersawVoice: k1_pickup_raw_[i] = s->GetPortaTime01(); break;
        case KnobContext::SupersawChorus:
            k1_pickup_raw_[i] = s->GetChorusDepth01();
            k2_pickup_raw_[i] = s->GetChorusRate01();
            break;
        case KnobContext::SupersawMix:
            k1_pickup_raw_[i] = s->GetOutputLevel01();
            k2_pickup_raw_[i] = s->GetGain01();
            break;
        case KnobContext::SupersawFxReverb:
            k1_pickup_raw_[i] = s->GetReverbSend01();
            k2_pickup_raw_[i] = reverb_size01_;
            break;
        case KnobContext::SupersawFxDelay:
            k1_pickup_raw_[i] = s->GetDelaySend01();
            k2_pickup_raw_[i] = delay_time01_;
            break;
        case KnobContext::SupersawMidi:
            k1_pickup_raw_[i] = Clamp(((float)(supersaw_midi_channel_ + 1) + 0.5f) / 16.99f, 0.f, 1.f);
            k2_pickup_raw_[i] = Clamp(((float)(s->GetMaxVoices() - 1) + 0.5f)
                                          / ((float)E::kMaxVoices - 0.01f),
                                      0.f, 1.f);
            break;
        case KnobContext::SupersawMidiVel: k1_pickup_raw_[i] = s->GetVelSens01(); break;
        default: break;
    }
}

void Ui::SupersawApplyKnobs(float k1, float k2, size_t ci)
{
    SupersawSynth* s = supersaw_;
    if(!s)
        return;
    using E = SupersawSynth;
    switch(supersaw_page_)
    {
        case SupersawParamPage::SuperSaw:
            if(PickUpK1(k1))
                s->SetDetune01(k1);
            if(PickUpK2(k2))
                s->SetMix01(k2);
            break;
        case SupersawParamPage::Osc2:
            if(PickUpK1(k1))
                s->SetOsc2Coarse(-24 + (int)(Clamp(k1, 0.f, 1.f) * 48.99f));
            if(PickUpK2(k2))
                s->SetOsc2Fine(-50.f + Clamp(k2, 0.f, 1.f) * 100.f);
            break;
        case SupersawParamPage::Balance:
            if(PickUpK1(k1))
                s->SetBalance01(k1);
            break;
        case SupersawParamPage::Filter:
            if(PickUpK1(k1))
                s->SetCutoff01(k1);
            if(PickUpK2(k2))
                s->SetResonance01(k2);
            break;
        case SupersawParamPage::FltEnv:
        case SupersawParamPage::AmpEnv:
        {
            const bool flt = supersaw_page_ == SupersawParamPage::FltEnv;
            const int  e   = flt ? E::kFilterEnv : E::kAmpEnv;
            if(!(flt ? ssw_fenv_sr_ : ssw_aenv_sr_))
            {
                if(PickUpK1(k1))
                    s->SetAttack01(e, k1);
                if(PickUpK2(k2))
                    s->SetDecay01(e, k2);
            }
            else
            {
                if(PickUpK1(k1))
                    s->SetSustain01(e, k1);
                if(PickUpK2(k2))
                    s->SetRelease01(e, k2);
            }
            break;
        }
        case SupersawParamPage::FltMod:
            if(PickUpK1(k1))
                s->SetFilterEnvDepth01(k1);
            if(PickUpK2(k2))
                s->SetKeyFollow01(k2);
            break;
        case SupersawParamPage::Lfo:
            LfoApplyKnobs(s->Lfo(), false, &ssw_lfo_dest_, k1, k2, ci);
            break;
        case SupersawParamPage::LfoDepth:
            LfoApplyKnobs(s->Lfo(), true, &ssw_lfo_dest_, k1, k2, ci);
            break;
        case SupersawParamPage::Voice:
            if(PickUpK1(k1))
                s->SetPortaTime01(k1);
            break;
        case SupersawParamPage::Chorus:
            if(PickUpK1(k1))
                s->SetChorusDepth01(k1);
            if(PickUpK2(k2))
                s->SetChorusRate01(k2);
            break;
        case SupersawParamPage::Mix: // Pan is on the Mixer
            if(PickUpK1(k1))
                s->SetOutputLevel01(k1);
            if(PickUpK2(k2))
                s->SetGain01(k2);
            break;
        case SupersawParamPage::FX:
            if(!ssw_fx_delay_)
            {
                if(PickUpK1(k1))
                    s->SetReverbSend01(Clamp(k1, 0.f, 1.f));
                if(PickUpK2(k2))
                    reverb_size01_ = Clamp(k2, 0.f, 1.f);
            }
            else
            {
                if(PickUpK1(k1))
                    s->SetDelaySend01(Clamp(k1, 0.f, 1.f));
                if(PickUpK2(k2))
                    delay_time01_ = Clamp(k2, 0.f, 1.f);
            }
            break;
        case SupersawParamPage::MIDI:
            if(ssw_midi_vel_)
            {
                if(PickUpK1(k1))
                    s->SetVelSens01(k1);
                break;
            }
            if(PickUpK1(k1))
                supersaw_midi_channel_ = -1 + (int)(Clamp(k1, 0.f, 1.f) * 16.99f);
            if(PickUpK2(k2))
                s->SetMaxVoices(1 + (int)(Clamp(k2, 0.f, 1.f) * ((float)E::kMaxVoices - 0.01f)));
            break;
        case SupersawParamPage::Preset:
        {
            // K2 scrolls the factory sounds (shared browse convention).
            const int total = E::GetNumFactoryPresets();
            if(save_load_mode_ == SaveLoadMode::BrowsingLoad && total > 0)
            {
                int idx = (int)(Clamp(k2, 0.f, 1.f) * total);
                if(idx >= total)
                    idx = total - 1;
                if(idx != ssw_preset_cursor_)
                    ssw_status_[0] = '\0';
                ssw_preset_cursor_ = idx;
            }
            break;
        }
        case SupersawParamPage::User:
            if(save_load_mode_ == SaveLoadMode::ChoosingSave)
                // Overwrite only makes sense once a user save is loaded.
                save_as_new_ = !SswIsUserSlot(ssw_loaded_slot_) ? true : k1 >= 0.5f;
            else if(save_load_mode_ == SaveLoadMode::BrowsingLoad)
            {
                const int total = 1 + ssw_user_count_; // "New" + saves
                int       idx   = (int)(Clamp(k2, 0.f, 1.f) * total);
                if(idx >= total)
                    idx = total - 1;
                if(idx != ssw_user_cursor_)
                    ssw_status_[0] = '\0';
                ssw_user_cursor_ = idx;
            }
            break;
        default: break;
    }
}

// --- Buttons -----------------------------------------------------------

void Ui::SupersawButton1()
{
    SupersawSynth* s = supersaw_;
    if(!s)
        return;
    using E = SupersawSynth;
    switch(supersaw_page_)
    {
        case SupersawParamPage::Osc2:
            s->SetOsc2Wave((E::Osc2Wave)(((int)s->GetOsc2Wave() + 1) % (int)E::Osc2Wave::kCount));
            break;
        case SupersawParamPage::Filter:
            s->SetFilterType(
                (E::FilterType)(((int)s->GetFilterType() + 1) % (int)E::FilterType::kCount));
            break;
        case SupersawParamPage::FltEnv: ssw_fenv_sr_ = false; break; // knobs -> Attack+Decay
        case SupersawParamPage::AmpEnv: ssw_aenv_sr_ = false; break;
        case SupersawParamPage::Lfo: LfoCycleWave(s->Lfo()); break;
        case SupersawParamPage::Voice:
            s->SetVoiceMode(
                (E::VoiceMode)(((int)s->GetVoiceMode() + 1) % (int)E::VoiceMode::kCount));
            break;
        case SupersawParamPage::FX: ssw_fx_delay_ = false; break;  // knobs -> Reverb
        case SupersawParamPage::MIDI: s->CycleModWheelTarget(); break;
        case SupersawParamPage::Preset:
            if(save_load_mode_ != SaveLoadMode::Idle)
                save_load_mode_ = SaveLoadMode::Idle; // Back
            break;
        case SupersawParamPage::User:
            if(save_load_mode_ != SaveLoadMode::Idle)
                save_load_mode_ = SaveLoadMode::Idle; // Back
            else if(!PerformanceStore::IsCardPresent())
                PerformanceStore::Remount();
            else
            {
                // Reveal Overwrite / Save New -- K1 picks, holding Button2
                // confirms (see SupersawHandleButton2()).
                save_load_mode_ = SaveLoadMode::ChoosingSave;
                save_as_new_    = !SswIsUserSlot(ssw_loaded_slot_);
            }
            break;
        default: break;
    }
}

void Ui::SupersawButton2Short()
{
    SupersawSynth* s = supersaw_;
    if(!s)
        return;
    switch(supersaw_page_)
    {
        case SupersawParamPage::Filter: s->SetFilter24dB(!s->GetFilter24dB()); break;
        case SupersawParamPage::FltEnv: ssw_fenv_sr_ = true; break; // knobs -> Sustain+Release
        case SupersawParamPage::AmpEnv: ssw_aenv_sr_ = true; break;
        case SupersawParamPage::Lfo: LfoToggleSync(s->Lfo(), KnobContext::SupersawLfo); break;
        case SupersawParamPage::Voice: s->SetPortamento(!s->GetPortamento()); break;
        case SupersawParamPage::FX: ssw_fx_delay_ = true; break;   // knobs -> Delay
        case SupersawParamPage::MIDI: ssw_midi_vel_ = !ssw_midi_vel_; break;
        default: break;
    }
}

// Preset/User: the Ensemble's convention -- Preset: tap opens the list, a
// tap loads and stays. User: tap opens the list (entry 0 is New), saving
// is an 800 ms hold. Returns false on any other page.
bool Ui::SupersawHandleButton2(const UiControlEvents& events)
{
    Switch& b = pod_->button2;
    if(supersaw_page_ == SupersawParamPage::Preset)
    {
        if(events.btn2_released)
        {
            if(save_load_mode_ == SaveLoadMode::BrowsingLoad)
                LoadSupersawSlot(kSswFactoryBase + ssw_preset_cursor_);
            else
                save_load_mode_ = SaveLoadMode::BrowsingLoad;
            button2_long_fired_ = false;
        }
        return true;
    }
    if(supersaw_page_ != SupersawParamPage::User)
        return false;
    const bool can_save = save_load_mode_ == SaveLoadMode::ChoosingSave;
    if(b.Pressed() && b.TimeHeldMs() > 800.f && !button2_long_fired_ && can_save)
    {
        button2_long_fired_ = true;
        TriggerSaveSupersawPreset(save_as_new_);
        save_load_mode_ = SaveLoadMode::Idle;
    }
    if(events.btn2_released)
    {
        if(!button2_long_fired_ && events.btn2_held_ms > 800.f && can_save)
        {
            TriggerSaveSupersawPreset(save_as_new_);
            save_load_mode_ = SaveLoadMode::Idle;
        }
        else if(!button2_long_fired_ && save_load_mode_ == SaveLoadMode::BrowsingLoad)
        {
            if(ssw_user_cursor_ == 0)
                TriggerNewSupersawPreset();
            else if(ssw_user_cursor_ - 1 < ssw_user_count_)
                LoadSupersawSlot(ssw_user_slots_[ssw_user_cursor_ - 1]);
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
    return true;
}

// --- Presets -----------------------------------------------------------

void Ui::RefreshSupersawPresetSlots()
{
    ssw_user_count_ = PerformanceStore::ListSupersawPresets(ssw_user_slots_, kMaxSswSlots);
    if(ssw_user_cursor_ > ssw_user_count_)
        ssw_user_cursor_ = ssw_user_count_;
    ssw_slots_dirty_ = false;
}

void Ui::SupersawPresetName(int slot, char* out, size_t out_size) const
{
    if(slot <= 0)
        snprintf(out, out_size, "New");
    else if(SswIsFactorySlot(slot))
        snprintf(out, out_size, "%s", SupersawSynth::GetFactoryPresetName(slot - kSswFactoryBase));
    else
        snprintf(out, out_size, "SSAW%d", slot);
}

void Ui::TriggerSaveSupersawPreset(bool force_new)
{
    if(!supersaw_)
        return;
    // Overwrite the loaded user save; a factory sound or nothing loaded
    // has none, so it goes to a new slot.
    const int slot = (!force_new && SswIsUserSlot(ssw_loaded_slot_))
                         ? ssw_loaded_slot_
                         : PerformanceStore::NextFreeSupersawPresetSlot();
    if(slot < 0)
    {
        snprintf(ssw_status_, sizeof(ssw_status_), "Card full/missing");
        return;
    }
    SupersawSynth::SupersawPresetData preset = supersaw_->CapturePreset();
    preset.midi_channel                      = supersaw_midi_channel_;
    g_audio_suspended = true; // see Ui::TriggerSave(): a short blocking SD write
    const bool ok     = PerformanceStore::SaveSupersawPreset(slot, preset);
    g_audio_suspended = false;
    if(ok)
    {
        ssw_loaded_slot_ = slot;
        snprintf(ssw_status_, sizeof(ssw_status_), "Saved %d", slot);
        MarkSupersawClean();
        ssw_slots_dirty_ = true;
    }
    else
        snprintf(ssw_status_, sizeof(ssw_status_), "Fail:%s", PerformanceStore::GetLastError());
}

void Ui::TriggerNewSupersawPreset()
{
    if(!supersaw_)
        return;
    supersaw_->ApplyPreset(supersaw_->GetNewPreset());
    ssw_loaded_slot_ = 0;
    snprintf(ssw_status_, sizeof(ssw_status_), "New");
    MarkSupersawClean();
}

void Ui::LoadSupersawSlot(int slot)
{
    if(!supersaw_ || slot < 1)
        return;
    const bool                        factory = SswIsFactorySlot(slot);
    SupersawSynth::SupersawPresetData preset;
    const bool ok = factory ? true : PerformanceStore::LoadSupersawPreset(slot, &preset);
    if(factory)
        preset = SupersawSynth::GetFactoryPreset(slot - kSswFactoryBase);
    if(!ok)
    {
        snprintf(ssw_status_, sizeof(ssw_status_), "Fail:%s", PerformanceStore::GetLastError());
        return;
    }
    supersaw_->ApplyPreset(preset);
    ApplyPresetMidiChannel(preset.midi_channel, &supersaw_midi_channel_);
    ssw_loaded_slot_ = slot;
    char name[24];
    SupersawPresetName(slot, name, sizeof(name));
    snprintf(ssw_status_, sizeof(ssw_status_), "Loaded %s", name);
    MarkSupersawClean();
}

Ui::EditSnapshot Ui::SupersawSnapshot() const
{
    EditSnapshot s;
    if(!supersaw_)
        return s;
    const SupersawSynth::SupersawPresetData p = supersaw_->CapturePreset();
    s.Add(p);
    s.Add(supersaw_midi_channel_);
    return s;
}

// --- Drawing -----------------------------------------------------------

void Ui::DrawSupersawScreen()
{
    SupersawSynth* s = supersaw_;
    if(!s)
        return;
    using E = SupersawSynth;
    static const char* const kNames[(int)SupersawParamPage::kCount]
        = {"SSaw", "Osc2",  "Balance", "Filter", "FltEnv", "FltMod", "AmpEnv", "LFO",
           "LFODep", "Voice", "Chorus", "Mix",   "FX",     "MIDI",   "Preset", "User"};
    char title[24];
    snprintf(title, sizeof(title), "SSaw:%s", kNames[(int)supersaw_page_]);
    DrawPageTitle(title, SupersawUnsaved());
    DrawBeatIndicator(disp_->Width() - 41, 0, 3);
    disp_->DrawLine(0, 9, disp_->Width() - 1, 9, true);

    char v1[16], v2[16], line[32];
    switch(supersaw_page_)
    {
        case SupersawParamPage::SuperSaw:
        {
            // The seven saws as bars on a scale: across, spread with the
            // Detune knob (the JP's lopsided spacing, so the bars move as
            // the knob does -- its true pitch spread climbs steeply only at
            // the top, so that's shown as "Spread xx ct" instead); up, each
            // saw's level (the centre's fixed, the sides rise with Mix).
            // The centre saw is solid, the outer pair dotted.
            static const int kOffsets[7] = {0, 128, -128, 408, -412, 704, -720};
            const float      det         = s->GetDetune01();
            const float      side        = (1.f + 32.f * s->GetMix01()) / 33.f;
            constexpr int    kMid = 64, kHalf = 55, kY0 = 20, kY1 = 39, kRuler = 41, kMinGap = 5;
            disp_->DrawLine(kMid - kHalf, kRuler, kMid + kHalf, kRuler, true);
            static const int kTicks[5] = {9, 36, 64, 92, 119};
            for(int t : kTicks)
                disp_->DrawLine(t, kRuler, t, kRuler + 2, true);
            float pos[7];
            for(int k = 0; k < 7; k++)
                pos[k] = (float)kMid + (float)kOffsets[k] / 720.f * (float)kHalf * det;
            // Keep each bar at least kMinGap from its neighbour, working out
            // from the centre, so all seven stay visible at low Detune.
            static const int kRight[3] = {1, 3, 5}, kLeft[3] = {2, 4, 6};
            float prev_r = pos[0], prev_l = pos[0];
            for(int i = 0; i < 3; i++)
            {
                if(pos[kRight[i]] - prev_r < (float)kMinGap)
                    pos[kRight[i]] = prev_r + (float)kMinGap;
                prev_r = pos[kRight[i]];
                if(prev_l - pos[kLeft[i]] < (float)kMinGap)
                    pos[kLeft[i]] = prev_l - (float)kMinGap;
                prev_l = pos[kLeft[i]];
            }
            for(int k = 0; k < 7; k++)
            {
                const int   x   = (int)(pos[k] + 0.5f);
                const float h   = k == 0 ? 25.f / 33.f : side;
                const int   top = kY1 - (int)((float)(kY1 - kY0) * h);
                if(k == 0)
                    disp_->DrawRect(x - 2, top, x + 2, kY1, true, true);
                else if(k >= 5) // the outer pair, dotted
                {
                    for(int y = top; y <= kY1; y += 2)
                    {
                        disp_->DrawPixel(x - 1, y, true);
                        disp_->DrawPixel(x + 1, y, true);
                    }
                    disp_->DrawPixel(x, top, true);
                }
                else
                    disp_->DrawRect(x - 1, top, x + 1, kY1, true, false);
            }
            snprintf(line, sizeof(line), "Spread %dct", (int)(s->GetSpreadCents() + 0.5f));
            TomThumbDrawText(disp_, 2, 16, line, true);
            Pct(s->GetDetune01(), v1, sizeof(v1));
            Pct(s->GetMix01(), v2, sizeof(v2));
            DrawControlRow(kFooterRow1Y, false, kFooterDividerY, "Detune", v1, v2, "Mix");
            DrawControlRow(kFooterRow2Y, true, kFooterInterRowDividerY, "", "", "", "");
            break;
        }
        case SupersawParamPage::Osc2:
        {
            // One cycle of its wave.
            const int  x0 = 20, x1 = disp_->Width() - 21, yt = 16, yb = kFooterDividerY - 6;
            const auto wave = s->GetOsc2Wave();
            int        py   = -1;
            for(int x = x0; x <= x1; x++)
            {
                const float p = (float)(x - x0) / (float)(x1 - x0);
                float       y01;
                if(wave == E::Osc2Wave::Saw)
                    y01 = 1.f - p;
                else if(wave == E::Osc2Wave::Square)
                    y01 = p < 0.5f ? 1.f : 0.f;
                else
                    y01 = p < 0.5f ? 2.f * p : 2.f - 2.f * p;
                const int y = yb - (int)((float)(yb - yt) * y01 + 0.5f);
                disp_->DrawLine(x, py < 0 ? y : py, x, y, true);
                py = y;
            }
            snprintf(v1, sizeof(v1), "%+dst", s->GetOsc2Coarse());
            snprintf(v2, sizeof(v2), "%+dct", (int)lroundf(s->GetOsc2Fine()));
            DrawControlRow(kFooterRow1Y, false, kFooterDividerY, "Coarse", v1, v2, "Fine");
            snprintf(line, sizeof(line), "Wave:%s", kOsc2Waves[(int)wave]);
            DrawControlRow(kFooterRow2Y, true, kFooterInterRowDividerY, line, "", "", "");
            break;
        }
        case SupersawParamPage::Balance:
        {
            // Each oscillator's level as a bar.
            const float b  = s->GetBalance01();
            const float g1 = b < 0.5f ? 1.f : 2.f * (1.f - b);
            const float g2 = b > 0.5f ? 1.f : 2.f * b;
            const int   w  = disp_->Width() - 40;
            TomThumbDrawText(disp_, 2, 21, "OSC1", true);
            TomThumbDrawText(disp_, 2, 35, "OSC2", true);
            DrawMixerHBar(24, 16, w, 6, g1);
            DrawMixerHBar(24, 30, w, 6, g2);
            Pct(b, v1, sizeof(v1));
            DrawControlRow(kFooterRow1Y, false, kFooterDividerY, "Balance", v1, "", "");
            DrawControlRow(kFooterRow2Y, true, kFooterInterRowDividerY, "", "", "", "");
            break;
        }
        case SupersawParamPage::Filter:
        {
            static const FilterMode kModes[3]
                = {FilterMode::LowPass, FilterMode::HighPass, FilterMode::BandPass};
            DrawFilterCurve(kModes[(int)s->GetFilterType()], s->GetCutoff01(), s->GetResonance01());
            Pct(s->GetCutoff01(), v1, sizeof(v1));
            Pct(s->GetResonance01(), v2, sizeof(v2));
            DrawControlRow(kFooterRow1Y, false, kFooterDividerY, "Cutoff", v1, v2, "Res");
            snprintf(line, sizeof(line), "Type:%s", kFilterTypes[(int)s->GetFilterType()]);
            DrawControlRow(kFooterRow2Y, true, kFooterInterRowDividerY, line, "", "", "");
            DrawStarToggle(false, "12/24dB", s->GetFilter24dB());
            break;
        }
        case SupersawParamPage::FltEnv:
        case SupersawParamPage::AmpEnv:
        {
            const bool flt = supersaw_page_ == SupersawParamPage::FltEnv;
            const int  e   = flt ? E::kFilterEnv : E::kAmpEnv;
            const bool sr  = flt ? ssw_fenv_sr_ : ssw_aenv_sr_;
            DrawAdsrShape(12, kFooterDividerY - 2, E::EnvSeconds(s->GetAttack01(e)),
                          E::EnvSeconds(s->GetDecay01(e)), s->GetSustain01(e),
                          E::EnvSeconds(s->GetRelease01(e)));
            if(!sr)
            {
                Pct(s->GetAttack01(e), v1, sizeof(v1));
                Pct(s->GetDecay01(e), v2, sizeof(v2));
                DrawControlRow(kFooterRow1Y, false, kFooterDividerY, "Attack", v1, v2, "Decay");
            }
            else
            {
                Pct(s->GetSustain01(e), v1, sizeof(v1));
                Pct(s->GetRelease01(e), v2, sizeof(v2));
                DrawControlRow(kFooterRow1Y, false, kFooterDividerY, "Sustain", v1, v2, "Release");
            }
            DrawControlRow(kFooterRow2Y, true, kFooterInterRowDividerY, sr ? "AD" : "AD*", "", "",
                           sr ? "SR*" : "SR");
            break;
        }
        case SupersawParamPage::FltMod:
        {
            BiPct(s->GetFilterEnvDepth01(), v1, sizeof(v1));
            Pct(s->GetKeyFollow01(), v2, sizeof(v2));
            snprintf(line, sizeof(line), "Env depth %s", v1);
            TomThumbDrawText(disp_, 2, 20, line, true);
            snprintf(line, sizeof(line), "Key follow %s", v2);
            TomThumbDrawText(disp_, 2, 32, line, true);
            DrawControlRow(kFooterRow1Y, false, kFooterDividerY, "Depth", v1, v2, "KeyFlw");
            DrawControlRow(kFooterRow2Y, true, kFooterInterRowDividerY, "", "", "", "");
            break;
        }
        case SupersawParamPage::Lfo: DrawLfoPage(s->Lfo()); break;
        case SupersawParamPage::LfoDepth: DrawLfoDepthPage(s->Lfo(), ssw_lfo_dest_); break;
        case SupersawParamPage::Voice:
        {
            snprintf(line, sizeof(line), "Mode: %s", kVoiceModes[(int)s->GetVoiceMode()]);
            TomThumbDrawText(disp_, 2, 20, line, true);
            Secs(0.001f + s->GetPortaTime01() * 2.f, v1, sizeof(v1));
            snprintf(line, sizeof(line), "Portamento: %s", s->GetPortamento() ? "On" : "Off");
            TomThumbDrawText(disp_, 2, 32, line, true);
            DrawControlRow(kFooterRow1Y, false, kFooterDividerY, "Glide", v1, "", "");
            DrawControlRow(kFooterRow2Y, true, kFooterInterRowDividerY, "Cycle mode", "", "",
                           s->GetPortamento() ? "Porta:On" : "Porta:Off");
            break;
        }
        case SupersawParamPage::Chorus:
        {
            TomThumbDrawText(disp_, 2, 24, "Stereo chorus", true);
            Pct(s->GetChorusDepth01(), v1, sizeof(v1));
            Pct(s->GetChorusRate01(), v2, sizeof(v2));
            DrawControlRow(kFooterRow1Y, false, kFooterDividerY, "Depth", v1, v2, "Rate");
            DrawControlRow(kFooterRow2Y, true, kFooterInterRowDividerY, "", "", "", "");
            break;
        }
        case SupersawParamPage::Mix:
        {
            // The live meter, as on Global:Mixer.
            DrawMixerHBar(2, 22, disp_->Width() - 4, 8, MeterBar01(MixerGetLevel01(kMixSsw)),
                          MixerIsClipping(kMixSsw));
            // K1 Level, K2 Gain into the limiter; Pan is on the Mixer.
            Pct(s->GetOutputLevel01(), v1, sizeof(v1));
            GainDb(s->GetGain01(), v2, sizeof(v2));
            DrawControlRow(kFooterRow1Y, false, kFooterDividerY, "Level", v1, v2, "Gain");
            DrawControlRow(kFooterRow2Y, true, kFooterInterRowDividerY, "", "", "", "");
            break;
        }
        case SupersawParamPage::FX:
        {
            if(!ssw_fx_delay_)
            {
                Pct(s->GetReverbSend01(), v1, sizeof(v1));
                Pct(reverb_size01_, v2, sizeof(v2));
                DrawControlRow(kFooterRow1Y, false, kFooterDividerY, "Send", v1, v2, "Size");
            }
            else
            {
                Pct(s->GetDelaySend01(), v1, sizeof(v1));
                FormatDelayTime(delay_time01_, v2, sizeof(v2));
                DrawControlRow(kFooterRow1Y, false, kFooterDividerY, "Send", v1, v2, "Time");
            }
            DrawFxSends(s->GetReverbSend01(), s->GetDelaySend01(), ssw_fx_delay_ ? 1 : 0);
            DrawControlRow(kFooterRow2Y, true, kFooterInterRowDividerY,
                           ssw_fx_delay_ ? "Reverb" : "Reverb*", "", "",
                           ssw_fx_delay_ ? "Delay*" : "Delay");
            break;
        }
        case SupersawParamPage::MIDI:
        {
            char chan[8], vel[8];
            if(supersaw_midi_channel_ < 0)
                snprintf(chan, sizeof(chan), "Omni");
            else
                snprintf(chan, sizeof(chan), "%d", supersaw_midi_channel_ + 1);
            Pct(s->GetVelSens01(), vel, sizeof(vel));
            snprintf(line, sizeof(line), "MIDI channel: %s", chan);
            TomThumbDrawText(disp_, 2, 17, line, true);
            snprintf(line, sizeof(line), "Wheel: %s", s->GetModWheelTargetName());
            TomThumbDrawText(disp_, 2, 27, line, true);
            snprintf(line, sizeof(line), "Velocity: %s", s->GetVelSens01() > 0.f ? vel : "Off");
            TomThumbDrawText(disp_, 2, 37, line, true);
            if(ssw_midi_vel_)
                DrawControlRow(kFooterRow1Y, false, kFooterDividerY, "Velocity", vel, "", "");
            else
            {
                snprintf(v2, sizeof(v2), "%d", s->GetMaxVoices());
                DrawControlRow(kFooterRow1Y, false, kFooterDividerY, "MIDI Ch", chan, v2, "Voices");
            }
            DrawControlRow(kFooterRow2Y, true, kFooterInterRowDividerY, "Cycle wheel tgt", "", "", "");
            DrawStarToggle(false, "Ch/Vel", ssw_midi_vel_);
            break;
        }
        case SupersawParamPage::Preset:
        case SupersawParamPage::User:
        {
            const bool is_user = supersaw_page_ == SupersawParamPage::User;
            if(is_user && !PerformanceStore::IsCardPresent())
            {
                disp_->SetCursor(0, 20);
                WriteUpper("No card");
                DrawControlRow(kFooterRow1Y, false, kFooterDividerY, "", "", "", "");
                DrawControlRow(kFooterRow2Y, true, kFooterInterRowDividerY, "Retry", "", "", "");
                break;
            }
            if(is_user && ssw_slots_dirty_)
                RefreshSupersawPresetSlots();
            const bool choosing_save = save_load_mode_ == SaveLoadMode::ChoosingSave;
            const bool browsing_load = save_load_mode_ == SaveLoadMode::BrowsingLoad;
            if(pod_->button2.Pressed() && choosing_save)
            {
                const float held = pod_->button2.TimeHeldMs();
                const int   w    = (int)(Clamp(held / 800.f, 0.f, 1.f) * (disp_->Width() - 2));
                disp_->SetCursor(0, 20);
                WriteUpper("Hold: Save...");
                disp_->DrawRect(0, 30, disp_->Width() - 1, 34, true, false);
                if(w > 0)
                    disp_->DrawRect(1, 31, w, 33, true, true);
                DrawControlRow(kFooterRow1Y, false, kFooterDividerY, "", "", "", "");
                DrawControlRow(kFooterRow2Y, true, kFooterInterRowDividerY, "", "", "", "");
                break;
            }
            TomThumbDrawText(disp_, 0, 15, is_user ? "User Presets" : "Factory Presets", true);
            if(choosing_save)
            {
                const bool can_overwrite = SswIsUserSlot(ssw_loaded_slot_);
                char       l1[24], l2[16];
                snprintf(l1, sizeof(l1), "%c Overwrite%s", !save_as_new_ ? '>' : ' ',
                         can_overwrite ? "" : " (n/a)");
                snprintf(l2, sizeof(l2), "%c Save New", save_as_new_ ? '>' : ' ');
                disp_->SetCursor(0, 24);
                WriteUpper(l1);
                disp_->SetCursor(0, 34);
                WriteUpper(l2);
            }
            else if(browsing_load)
            {
                // "> " marks the sound that's loaded.
                int  slot = 0;
                char name[24];
                if(!is_user)
                    slot = kSswFactoryBase + ssw_preset_cursor_;
                else if(ssw_user_cursor_ > 0)
                    slot = ssw_user_slots_[ssw_user_cursor_ - 1];
                if(is_user && ssw_user_cursor_ == 0)
                    snprintf(line, sizeof(line), "New (blank)");
                else
                {
                    SupersawPresetName(slot, name, sizeof(name));
                    snprintf(line, sizeof(line), "%s%s", slot == ssw_loaded_slot_ ? "> " : "", name);
                }
                disp_->SetCursor(0, 24);
                WriteUpper(line);
            }
            else
            {
                char name[24];
                if(ssw_loaded_slot_ <= 0)
                    snprintf(line, sizeof(line), "Now: (new)");
                else
                {
                    SupersawPresetName(ssw_loaded_slot_, name, sizeof(name));
                    snprintf(line, sizeof(line), "Now: %s", name);
                }
                disp_->SetCursor(0, 24);
                WriteUpper(line);
                if(ssw_status_[0] != '\0')
                {
                    disp_->SetCursor(0, 36);
                    WriteUpper(ssw_status_);
                }
            }
            const char* b1 = save_load_mode_ != SaveLoadMode::Idle ? "Back" : is_user ? "Save" : "";
            DrawControlRow(kFooterRow1Y, false, kFooterDividerY, choosing_save ? "Select" : "", "",
                           "", browsing_load ? "Scroll" : "");
            DrawControlRow(kFooterRow2Y, true, kFooterInterRowDividerY, b1, "", "",
                           choosing_save ? "Hold=Save" : "Load");
            break;
        }
        default: break;
    }
}
