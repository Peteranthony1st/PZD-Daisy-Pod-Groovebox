#include <cstring>
#include "svf_setup.h"
#include "daisy_pod.h"
#include "daisysp.h"
#include "dev/oled_ssd130x.h"
#include "tempo_clock.h"
#include "looper_layer.h"
#include "ui.h"
#include "performance_store.h"
#include "audio_engine.h"
#include "shared_delay.h"
#include "itcm.h"
#include "granular_engine.h"
#include "dexed_synth.h"
#include "ensemble_synth.h"
#include "sample_synth.h"
#include "dattorro_reverb.h"

using namespace daisy;

// Same sizing as the original Ouroboros firmware: ~33 seconds per layer
// at 48kHz, stereo. 4 layers (dropped from the original 5) to keep the
// per-layer SDRAM budget (Phaser per layer, plus one shared ReverbSc bus
// -- see fx_reverb_shared below) comfortable. Most per-layer effect
// objects are a few hundred bytes and live in ordinary SRAM via the
// LooperLayer array below -- Phaser and ReverbSc are the exceptions
// (~75KB/~386KB per instance), and are placed in SDRAM alongside these
// loop buffers.
//
// 921,600 (~19.2s @ 48kHz) -- was 1,600,000 (~33.3s). TempoClock's own
// BPM/Bars ceiling (see tempo_clock.h's kMaxLoopSeconds) now guarantees no
// new recording can ever exceed 19.2s, so sizing these for the old ~33.3s
// max was pure headroom nobody could reach any more -- shrinking them
// frees ~20.7MB of SDRAM project-wide, more than enough to cover the new
// shared Undo backup buffer below (~7.03MB) with plenty to spare.
#define kBuffSize 921600
#define kNumLayers 4

DaisyPod hw;

// True hardware PWM for 3 of the Pod's 6 RGB LED sub-pixels (LED1's Green/
// Blue, LED2's Red) -- the STM32H750's own TIM3 peripheral toggles these
// GPIOs in hardware every cycle, with zero CPU involvement per cycle (only
// a register write when the duty cycle actually changes), genuinely
// jitter-free unlike libDaisy's own software Led::Update() (a fixed-rate
// GPIO toggle from ControlTimerCallback(), itself at a lower NVIC priority
// than the audio DMA ISR and so not perfectly immune to its own timing
// jitter -- see that callback's own comment, since removed, for the full
// history). Only 3 of the 6 sub-pixels: per libDaisy's own per/pwm.h
// documented pin table, LED1-Green(D19)/LED1-Blue(D18)/LED2-Red(D17) all
// land on TIM3's own channels 1/2/4, but LED2-Green(D24) would need TIM5
// -- already claimed by ControlTimerCallback's own TimerHandle below, and
// per/pwm.h's own doc comment warns PWM and a TimerHandle can't share a
// timer -- and LED1-Red(D20)/LED2-Blue(D23) aren't wired to any TIM3/4/5
// channel libDaisy exposes at all. Those 3 unused sub-pixels are simply
// never driven again after DaisyPod::InitLeds()'s own ClearLeds() leaves
// them off, which is exactly the state this scheme wants for them anyway.
daisy::PWMHandle led_pwm_tim3;

using MyDisplay = daisy::OledDisplay<daisy::SSD130xI2c128x64Driver>;
MyDisplay display;

float DSY_SDRAM_BSS buffer_l[kNumLayers][kBuffSize];
float DSY_SDRAM_BSS buffer_r[kNumLayers][kBuffSize];

// Shared "undo last overdub" backup -- ONE instance, not one per layer
// (only one layer can be overdubbing at a time), restorable from
// Layer:Status's Button2 short tap (see Ui::HandleButton2()). Sized to
// exactly the same kMaxLoopSeconds ceiling as the main buffers above, so
// every loop this project can now produce always fits -- no partial/
// best-effort case to handle for anything recorded going forward. The
// actual copy happens incrementally, inside LooperLayer::Process()
// itself (the audio ISR), spread across the overdub's own count-in --
// see ConsumeOverdubSnapshotRequest()'s own comment for why it can't be
// deferred to this main loop. A loop from an old save made before this
// ceiling existed that happens to be longer than this is grandfathered
// (still plays fine) but simply won't offer Undo -- checked at the
// moment the snapshot would be taken, not assumed.
float DSY_SDRAM_BSS g_undo_backup_l[kBuffSize];
float DSY_SDRAM_BSS g_undo_backup_r[kBuffSize];
// -1 = no pending undo. Set by the main loop once a layer's incremental
// snapshot (see above) finishes; cleared either by a successful Undo or
// by a different layer's overdub taking the shared backup over first.
int    g_undo_owner_layer = -1;
size_t g_undo_backup_len  = 0;

// Grains' own dedicated capture buffer -- separate from the loop layers'
// buffers above (~10s @ 48kHz, chosen to fit comfortably in the SDRAM
// left over after the 4 loop layers -- raised from an original ~5s once
// that turned out too short for a real imported WAV file, e.g. a
// multi-bar commercial loop). Two sources write into it: Ui's
// TriggerGranularCaptureFromLayer() (an instant main-loop-side copy from
// whichever loop layer is selected) or live input recorded sample-by-
// sample in AudioCallback() while g_granular_capturing is true (see
// below). Either way, granular.SetSource() is pointed at it once the
// capture finishes, same as every other GranularEngine source.
constexpr size_t kGranularCaptureSamples = 480000; // ~10s @ 48kHz
float DSY_SDRAM_BSS g_granular_capture_l[kGranularCaptureSamples];
float DSY_SDRAM_BSS g_granular_capture_r[kGranularCaptureSamples];
// GranularEngine's own interleaved 16-bit copy of whatever it's playing
// (see GranularEngine::SetSourceShadow()) -- rebuilt on every SetSource().
int16_t DSY_SDRAM_BSS g_granular_shadow[kGranularCaptureSamples * 2];

// Shared between Ui (which starts/stops recording on Button2 press/
// release -- see Ui::HandleButton2()) and AudioCallback() (which actually
// writes live input samples in while true). volatile for the same
// ISR/main-loop-shared reason as every other cross-context flag in this
// file (g_encoder_pos etc.) -- Ui also clears write_pos to 0 right before
// setting this true, arming a fresh recording.
volatile bool   g_granular_capturing        = false;
volatile size_t g_granular_capture_write_pos = 0;

// SampleSynth's own SDRAM arena -- one kit's worth of sample audio at a
// time (Ui::TriggerLoadSampleSynthKit() owns the bump-allocator loop
// that fills this from WAV files on load; loading a new kit resets the
// write cursor and re-fills from the start). ~1,000,000 samples/channel
// (~20.8s @ 48kHz) -- generous for a 24-slot kit of short/medium hits.
constexpr size_t kSampleSynthArenaSamples = 1000000;
float DSY_SDRAM_BSS g_sample_synth_arena_l[kSampleSynthArenaSamples];
float DSY_SDRAM_BSS g_sample_synth_arena_r[kSampleSynthArenaSamples];

// SampleSynth's own dedicated capture buffer -- same "Direct Record/
// Import/From Layer" pattern as Grains' own g_granular_capture_l/r
// above, sized shorter (6s, not 10s) since this plays one-shots, not
// sustained/scanned texture.
constexpr size_t kSampleSynthCaptureSamples = 288000; // 6s @ 48kHz
float DSY_SDRAM_BSS g_sample_synth_capture_l[kSampleSynthCaptureSamples];
float DSY_SDRAM_BSS g_sample_synth_capture_r[kSampleSynthCaptureSamples];
volatile bool   g_sample_synth_capturing        = false;
volatile size_t g_sample_synth_capture_write_pos = 0;

TempoClock  tempo;
LooperLayer layers[kNumLayers];
Ui          ui;

// One Phaser per layer, in SDRAM because it's not small enough to live
// directly as a LooperLayer member in ordinary SRAM. Each layer's
// instance is fully independent (not shared) -- LooperLayer::Init()
// takes a pointer to it and owns calling Init() on it.
daisysp::Phaser DSY_SDRAM_BSS fx_phaser[kNumLayers];

// ONE shared reverb bus for every layer, not one per layer -- up to 4
// simultaneous ReverbSc instances (by far the heaviest thing in this
// signal chain, ~386KB each) was enough real-time DSP cost in the worst
// case to starve the main loop/TIM5 badly enough to cause actual audio
// glitches and OLED corruption, not just UI lag. Every layer's
// Reverb-Send-scaled signal is summed (see LooperLayer::Process()'s
// reverb_send_out parameter) and run through this single instance once
// per sample in AudioCallback() -- see fx_reverb_size01 below for how
// its shared Size control works. The 13 big delay buffers this needs
// (~166KB) live in fx_reverb_storage below -- moved from SDRAM into
// plain SRAM to test a real hypothesis: this is a memory-bound hot
// loop (13 delay-line/allpass/lowpass stages reading/writing those
// arrays every sample), so if SDRAM's own per-access latency was the
// dominant cost, SRAM's lower latency should cut real CPU, with zero
// audio-quality change (same algorithm, same constants, bit-identical
// output). Room for this was freed by moving ExportWav()/ImportWav()'s
// own scratch buffers (~196KB, performance_store.cpp) the other way
// into SDRAM -- those only ever run with the audio engine suspended,
// so SDRAM's extra latency costs nothing there. fx_reverb_shared
// itself only holds small, constantly-touched bookkeeping state
// (DelayBuffer structs, the "amount" coefficients), so it stays a
// plain object -- see DattorroReverbStorage's own doc comment for why
// this split exists.
DattorroReverb        fx_reverb_shared;
DattorroReverbStorage fx_reverb_storage;

// ONE shared cross-feedback ("ping-pong") delay, same "one shared
// instance, fed by per-engine sends" architecture as fx_reverb_shared
// above -- every engine/layer/Bypass has its own Delay Send into the
// SAME delay, set from Global:Delay/DlyTone. Cheap: two delay-line
// reads/writes, a one-pole filter and a few multiplies per sample (see
// SharedDelay::Bus). 3s per channel, ~1.15MB of SDRAM for both.
SharedDelay::Bus DSY_SDRAM_BSS fx_delay;

// Master-bus filter -- applied once to the final mix (layers + their
// reverb, summed) rather than per-layer, so it's a plain Svf pair
// living in ordinary SRAM (tiny, no big internal buffer like the
// per-layer effects above) instead of needing SDRAM placement.
TptSvf fx_master_filter_l, fx_master_filter_r;

// The MIDI-played granular voice (see granular_engine.h -- monophonic,
// two overlapping-grain layers). Small enough (Grain/Adsr/Svf state, no
// big internal buffer of its own) to live in ordinary SRAM -- it doesn't
// own the captured audio, just points at whichever buffer holds one (see
// g_granular_source_l/r below).
GranularEngine DSY_DTCM_BSS granular;

// The MIDI-played 6-operator/10-voice DX7 clone (see dexed_synth.h --
// real msfa engine underneath, own oldest-note-steal voice pool). Small
// enough (Dx7Note is ~700 bytes/voice per Synth_Dexed's own build) to
// live in ordinary SRAM.
DexedSynth DSY_DTCM_BSS dexed;

// A divide-down string-machine/ensemble-organ instrument (see
// ensemble_synth.h -- daisysp::OscillatorBank per voice, Dexed's own
// voice-stealing pattern). Measured on real hardware at ~22-23% peak CPU
// for 8 voices + the shared Chorus, well within budget alongside
// Dexed/Grains/the looper.
EnsembleSynth DSY_DTCM_BSS ensemble;

// A 10-voice polyphonic sample player (see sample_synth.h -- combines
// DexedSynth's own voice-stealing pattern with GranularEngine's
// pitch-tracking/clamp-at-ends buffer read). Ported from the `optimise`
// branch, where it was originally built alongside Dexed/Grains.
SampleSynth DSY_DTCM_BSS sample_synth;

// The engines above (and Dexed's FM lookup tables, see msfa/synth.h's
// TABLE_MEM) live in DTCMRAM -- see DSY_DTCM_BSS in itcm.h. Startup only
// zeroes ordinary .bss, so clear that section here, as the very first
// constructor (priority 101 runs before every default-priority global
// constructor), so every object there starts exactly as it would in .bss.
extern "C" uint32_t _sdtcmram_bss, _edtcmram_bss;
__attribute__((constructor(101))) static void ZeroDtcmBss()
{
    memset(&_sdtcmram_bss, 0,
           (size_t)((char*)&_edtcmram_bss - (char*)&_sdtcmram_bss));
}

// See audio_engine.h.
volatile bool g_audio_suspended = false;

// See audio_engine.h.
float g_grains_level  = 0.f;
float g_dexed_level   = 0.f;
float g_bypass_level  = 0.f;
float g_master_level  = 0.f;
float g_grains_clip_hold = 0.f;
float g_dexed_clip_hold  = 0.f;
float g_bypass_clip_hold = 0.f;
float g_master_clip_hold = 0.f;

// See audio_engine.h. daisy::CpuLoadMeter itself (libDaisy, pulled in
// transitively via daisy_pod.h) does the real measurement -- two
// separate instances rather than one, since GetAvgCpuLoad()'s own
// smoothing should run forever uninterrupted, but the peak reading
// needs its own periodic Reset() (see kCpuPeakResetSeconds below) and
// Reset() clears a CpuLoadMeter's avg_ right along with its min_/max_,
// so sharing one instance between both readings would reset the
// average too.
daisy::CpuLoadMeter g_cpu_meter_avg;
daisy::CpuLoadMeter g_cpu_meter_peak;
float               g_cpu_avg_load  = 0.f;
float               g_cpu_peak_load = 0.f;

// Same threshold/hold convention as LooperLayer's own clip_hold_ (see
// its doc comment) -- kept here rather than shared with it, since each
// one is reading a different signal.
constexpr float kMeterClipThreshold   = 1.f;
constexpr float kMeterClipHoldSeconds = 0.5f;
// How long the level bar takes to fall from a peak back toward silence
// -- real "peak meter" shape (instant attack, slow release), not a
// symmetric one-pole follower. A symmetric filter decays to ~1% within
// ~13 samples (~0.27ms @ 48kHz) either way, which is invisible on a
// ~30Hz OLED redraw for anything that isn't a continuous tone --
// exactly what made Grains' own bursty, gappy output (discrete grains
// separated by silence, depending on Fill/Gap) look like it barely
// moved even at a loud Mixer level: the bar had already decayed back to
// nothing between grain onsets, long before the next redraw ever read
// it. 300ms release is the same ballpark a real console VU meter uses.
constexpr float kMeterReleaseSeconds = 0.3f;

// Peak-hold level follower + clip-hold latch, shared shape for all 4 of
// the meters above -- factored out since it's the same lines x 4
// channels otherwise. Inlined in practice (DSY_ITCM_TEXT callers, tiny
// body), so this costs nothing over writing it out longhand each time.
static inline void UpdateLevelMeter(float& level, float& clip_hold, float sample,
                                      float sample_rate)
{
    float mag = fabsf(sample);
    if(mag > level)
        level = mag; // instant attack -- always show the true peak right away
    else
        level -= level * (1.f / (kMeterReleaseSeconds * sample_rate)); // slow release
    if(mag >= kMeterClipThreshold)
        clip_hold = kMeterClipHoldSeconds;
    else if(clip_hold > 0.f)
        clip_hold -= 1.f / sample_rate;
}

// How often the peak CPU reading rolls over -- short enough that
// turning a knob/toggling a feature shows its own new peak within a
// few seconds, rather than that number staying stuck at whatever the
// worst moment was several minutes ago. GetAvgCpuLoad() is NOT reset on
// this timer (see g_cpu_meter_avg/g_cpu_meter_peak's own comment for
// why they're two separate instances) -- only the peak reading rolls.
constexpr float kCpuPeakResetSeconds    = 10.f;
float           g_cpu_peak_reset_countdown = kCpuPeakResetSeconds;

// Call once at the very end of every AudioCallback() invocation --
// both the normal path and the g_audio_suspended early-return path (see
// their own call sites) -- so the reading stays live (correctly low)
// even across a Load/Save rather than freezing at whatever it was right
// before one started.
static inline void FinishCpuMeterBlock(size_t size, float sample_rate)
{
    g_cpu_meter_avg.OnBlockEnd();
    g_cpu_meter_peak.OnBlockEnd();
    g_cpu_avg_load  = g_cpu_meter_avg.GetAvgCpuLoad();
    g_cpu_peak_load = g_cpu_meter_peak.GetMaxCpuLoad();

    g_cpu_peak_reset_countdown -= (float)size / sample_rate;
    if(g_cpu_peak_reset_countdown <= 0.f)
    {
        g_cpu_meter_peak.Reset();
        g_cpu_peak_reset_countdown = kCpuPeakResetSeconds;
    }
}

// Written only by ControlTimerCallback (a 1kHz TIM5 ISR, see below), read
// only by the main loop, which diffs each against its own "last seen"
// value to build a UiControlEvents delta every iteration (see ui.h's
// UiControlEvents comment for why encoder/button edges need this
// treatment).
volatile int32_t  g_encoder_pos         = 0; // cumulative encoder position
volatile uint32_t g_encoder_click_falls = 0;
volatile uint32_t g_btn1_releases       = 0;
volatile uint32_t g_btn2_releases       = 0;

// Each button's held duration as of the most recent tick it was still
// pressed -- see ui.h's UiControlEvents::btn1_held_ms/btn2_held_ms
// comment for why this can't just be Switch::TimeHeldMs() read from the
// main loop. Naturally "freezes" at the correct value the instant a
// button releases (the write below only happens while Pressed() is
// still true), and stays there until the next press starts updating it
// again.
volatile float    g_btn1_held_ms        = 0.f;
volatile float    g_btn2_held_ms        = 0.f;

// Same convention, for GranularParamPage::Filter's own live oscilloscope.
constexpr size_t kGranularScopeSamples = 1024;
static float      g_granular_scope_l[kGranularScopeSamples];
static size_t      g_granular_scope_write_pos = 0;

// Same convention, for Screen::Dexed's own live oscilloscope pages
// (Feedback/Brightness/EnvSpeed/Filter/Mix/FX) -- captures Dexed's own
// dry output (dexed_l), same "instrument's own signal, not the final
// mix" treatment as the Granular scope above, so each page's own knobs
// (Feedback amount, Brightness, etc.) visibly change Dexed's actual
// output shape without other layers/instruments cluttering the trace.
constexpr size_t kDexedScopeSamples = 1024;
static float      g_dexed_scope_l[kDexedScopeSamples];
static size_t      g_dexed_scope_write_pos = 0;

// Same convention, for Screen::Ensemble's own live oscilloscope pages.
constexpr size_t kEnsembleScopeSamples = 1024;
static float      g_ensemble_scope_l[kEnsembleScopeSamples];
static size_t      g_ensemble_scope_write_pos = 0;

// Same convention, for Screen::SampleSynth's own live oscilloscope pages.
constexpr size_t kSampleSynthScopeSamples = 1024;
static float      g_sample_synth_scope_l[kSampleSynthScopeSamples];
static size_t      g_sample_synth_scope_write_pos = 0;

// Same convention, for Screen::Mixer's own oscilloscope page -- captures
// the actual final post-fader mix (after reverb/bypass/master filter/
// click/master volume), not any one instrument's own signal.
constexpr size_t kMasterScopeSamples = 1024;
static float      g_master_scope_l[kMasterScopeSamples];
static size_t      g_master_scope_write_pos = 0;

TimerHandle control_timer;

// Runs Debounce() on the encoder/buttons (and the knob ADC smoothing) at
// a guaranteed 1kHz, independent of whatever the main loop is doing --
// see ui.h's UiControlEvents comment. Cheap (a few GPIO/ADC reads), so
// safe to run from this low-priority timer ISR (see per/tim.cpp: TIM5
// is installed at NVIC priority 0x0f, i.e. below the audio DMA ISR).
void ControlTimerCallback(void*)
{
    hw.ProcessAllControls();
    g_encoder_pos += hw.encoder.Increment();
    if(hw.encoder.FallingEdge())
        g_encoder_click_falls++;
    // Switch::TimeHeldMs()/Pressed()/FallingEdge() are only reliable when
    // checked at the same rate Debounce() runs (see Switch's own doc
    // comment) -- true here (this whole callback is that same guaranteed
    // 1kHz tick), unlike reading them from the main loop's own irregular
    // iteration rate. Capturing the held duration every tick it's still
    // pressed (not just at the FallingEdge tick itself, when Pressed()
    // has *already* gone false and TimeHeldMs() would already read 0) is
    // what makes g_btn1_held_ms/g_btn2_held_ms trustworthy after the
    // fact.
    if(hw.button1.Pressed())
        g_btn1_held_ms = hw.button1.TimeHeldMs();
    if(hw.button1.FallingEdge())
        g_btn1_releases++;
    if(hw.button2.Pressed())
        g_btn2_held_ms = hw.button2.TimeHeldMs();
    if(hw.button2.FallingEdge())
        g_btn2_releases++;

    // MIDI polled here rather than the main loop for the same reason
    // buttons are: this runs at a guaranteed, jitter-free 1kHz regardless
    // of whatever the main loop's OLED redraw (blocking I2C) is doing.
    // Polled from the main loop instead, a redraw stall could delay
    // draining the UART long enough that a very fast tap's NoteOn AND
    // NoteOff both end up sitting in the queue together, getting drained
    // in the same pass with no audio block ever seeing the note as held
    // in between -- silence instead of a short blip. Same reasoning
    // applies to pitch bend: smoother, evenly-spaced bend_ratio_ updates
    // read far more natural than whatever's left of them after being
    // bunched up behind an OLED write.
    hw.midi.Listen();
    // -1 (Omni) matches any channel; otherwise the event's own 0-indexed
    // channel must match exactly -- see Ui::GetDexedMidiChannel()/
    // GetGranularMidiChannel()'s own doc comment. Read once per poll
    // (not once per event) since both can only change between UI
    // redraws anyway, not mid-event-queue.
    int dexed_ch        = ui.GetDexedMidiChannel();
    int granular_ch     = ui.GetGranularMidiChannel();
    int ensemble_ch     = ui.GetEnsembleMidiChannel();
    int sample_synth_ch = ui.GetSampleSynthMidiChannel();
    while(hw.midi.HasEvents())
    {
        MidiEvent event = hw.midi.PopEvent();
        if(event.type == NoteOn)
        {
            NoteOnEvent noteon = event.AsNoteOn();
            if(granular_ch == -1 || noteon.channel == granular_ch)
                granular.NoteOn(noteon.note, noteon.velocity);
            if(dexed_ch == -1 || noteon.channel == dexed_ch)
                dexed.NoteOn(noteon.note, noteon.velocity);
            if(ensemble_ch == -1 || noteon.channel == ensemble_ch)
                ensemble.NoteOn(noteon.note, noteon.velocity);
            if(sample_synth_ch == -1 || noteon.channel == sample_synth_ch)
                sample_synth.NoteOn(noteon.note, noteon.velocity);
        }
        else if(event.type == NoteOff)
        {
            NoteOffEvent noteoff = event.AsNoteOff();
            if(granular_ch == -1 || noteoff.channel == granular_ch)
                granular.NoteOff(noteoff.note);
            if(dexed_ch == -1 || noteoff.channel == dexed_ch)
                dexed.NoteOff(noteoff.note);
            if(ensemble_ch == -1 || noteoff.channel == ensemble_ch)
                ensemble.NoteOff(noteoff.note);
            if(sample_synth_ch == -1 || noteoff.channel == sample_synth_ch)
                sample_synth.NoteOff(noteoff.note);
        }
        else if(event.type == PitchBend)
        {
            // Grains has no pitch bend input (see DESIGN.md's own MIDI
            // section) -- Dexed and Ensemble both read this.
            // PitchBendEvent::value is already roughly centered near 0
            // (see MidiEvent::AsPitchBend()'s own -8192 offset), not the
            // raw 0..16383 CC value Dx7Note::compute()/EnsembleSynth::
            // SetPitchBend14bit() itself expect -- re-center and clamp
            // back to that range here.
            PitchBendEvent pb = event.AsPitchBend();
            if(dexed_ch == -1 || pb.channel == dexed_ch)
            {
                int32_t raw = (int32_t)pb.value + 8192;
                raw         = raw < 0 ? 0 : (raw > 16383 ? 16383 : raw);
                dexed.SetPitchBend14bit((uint16_t)raw);
            }
            if(ensemble_ch == -1 || pb.channel == ensemble_ch)
            {
                int32_t raw = (int32_t)pb.value + 8192;
                raw         = raw < 0 ? 0 : (raw > 16383 ? 16383 : raw);
                ensemble.SetPitchBend14bit((uint16_t)raw);
            }
            if(sample_synth_ch == -1 || pb.channel == sample_synth_ch)
            {
                int32_t raw = (int32_t)pb.value + 8192;
                raw         = raw < 0 ? 0 : (raw > 16383 ? 16383 : raw);
                sample_synth.SetPitchBend14bit((uint16_t)raw);
            }
        }
        else if(event.type == ControlChange)
        {
            ControlChangeEvent cc = event.AsControlChange();
            if(cc.control_number == 1) // mod wheel
            {
                // Grains has no mod-wheel input. Dexed's own target is
                // always one of Pitch/Amp/EgBias (no Off); Ensemble's
                // own defaults to Off (see EnsembleSynth::ModWheelTarget's
                // own comment) -- both read the same raw CC1 value
                // regardless, each engine's own target picks whether/how
                // it does anything with it.
                if(dexed_ch == -1 || cc.channel == dexed_ch)
                    dexed.SetModWheel01((float)cc.value / 127.f);
                if(ensemble_ch == -1 || cc.channel == ensemble_ch)
                    ensemble.SetModWheel01((float)cc.value / 127.f);
                // SampleSynth's own mod wheel drives a fixed pitch
                // vibrato (see SampleSynth::SetModWheel01()'s own
                // comment) -- no target concept to gate here, same as
                // Dexed's own always-on targets.
                if(sample_synth_ch == -1 || cc.channel == sample_synth_ch)
                    sample_synth.SetModWheel01((float)cc.value / 127.f);
            }
            // CC 123 (All Notes Off) / CC 120 (All Sound Off) -- see
            // DexedSynth::AllNotesOff()'s own doc comment for why this
            // exists at all (confirmed on hardware: an Akai MPK Mini
            // Plus's own arpeggiator can leave a note stuck without
            // this, same root cause across every engine here since they
            // all share this same plain NoteOn/NoteOff dispatch).
            else if(cc.control_number == 123 || cc.control_number == 120)
            {
                if(granular_ch == -1 || cc.channel == granular_ch)
                    granular.AllNotesOff();
                if(dexed_ch == -1 || cc.channel == dexed_ch)
                    dexed.AllNotesOff();
                if(ensemble_ch == -1 || cc.channel == ensemble_ch)
                    ensemble.AllNotesOff();
                if(sample_synth_ch == -1 || cc.channel == sample_synth_ch)
                    sample_synth.AllNotesOff();
            }
        }
    }
}

DSY_ITCM_TEXT
void AudioCallback(AudioHandle::InputBuffer  in,
                   AudioHandle::OutputBuffer out,
                   size_t                    size)
{
    static TempoClock::TempoTick ticks[256];
    static float                 click[256];
    // Every layer's Reverb-Send-scaled signal is summed here (see
    // LooperLayer::Process()'s reverb_send_out parameter), then run
    // through the ONE shared fx_reverb_shared once per sample below --
    // not once per layer any more, see fx_reverb_shared's comment.
    static float reverb_send_l[256];
    static float reverb_send_r[256];
    // Same idea, for the shared delay bus (fx_delay_l/r) further below.
    static float delay_send_l[256];
    static float delay_send_r[256];
    static float gran_l[256];
    static float gran_r[256];
    static float dexed_l[256];
    static float dexed_r[256];
    static float ensemble_l[256];
    static float ensemble_r[256];
    static float samp_l[256];
    static float samp_r[256];

    const float sample_rate = hw.AudioSampleRate(); // needed by both paths below
    g_cpu_meter_avg.OnBlockStart();
    g_cpu_meter_peak.OnBlockStart();

    if(g_audio_suspended)
    {
        for(size_t i = 0; i < size; i++)
        {
            out[0][i] = 0.f;
            out[1][i] = 0.f;
        }
        FinishCpuMeterBlock(size, sample_rate);
        return;
    }

    // Grains' Direct Record capture is applied further below, after
    // dexed_l/r are computed -- see g_granular_capturing's own comment
    // there for why.

    // Block-rate: tape-style multiplier on top of every layer's own Speed
    // and the tempo clock's own tick rate -- see Ui::GetProjectSpeed(),
    // TempoClock::Process(), LooperLayer::Process(). Read here, before
    // the loops below, since both need it.
    const float project_speed = ui.GetProjectSpeed(); // sample_rate already read above

    // Skipped along with the loop layers below when Global:Looper is off
    // -- ResetPhase() (called once, right when it's switched off) already
    // leaves bar/beat position, count-in state, and the click envelope at
    // a clean bar-1-beat-1 rest state, and not calling Process() here is
    // what keeps it sitting there (frozen, not just paused) instead of
    // silently continuing to advance in the background while disabled.
    bool looper_enabled = ui.IsLooperEnabled();
    for(size_t i = 0; i < size; i++)
    {
        out[0][i] = 0.f;
        out[1][i] = 0.f;
        reverb_send_l[i] = 0.f;
        reverb_send_r[i] = 0.f;
        delay_send_l[i]  = 0.f;
        delay_send_r[i]  = 0.f;

        if(looper_enabled)
        {
            ticks[i] = tempo.Process(project_speed);
            click[i] = tempo.RenderClick(ticks[i]);
        }
        else
        {
            ticks[i] = TempoClock::TempoTick{};
            click[i] = 0.f;
        }
    }

    // Skipped entirely when disabled from Global:Granular -- a real
    // compute saving, not just muting: gran_l/r stay at zero instead of
    // GranularEngine::Process() ever running.
    if(ui.IsGranularEnabled())
    {
        granular.Process(size, gran_l, gran_r, reverb_send_l, reverb_send_r, delay_send_l,
                          delay_send_r);
    }
    else
    {
        for(size_t i = 0; i < size; i++)
        {
            gran_l[i] = 0.f;
            gran_r[i] = 0.f;
        }
    }

    // Same for Granular's own oscilloscope (GranularParamPage::Filter),
    // and Screen::Mixer's own Grains level/clip meter -- same signal
    // either way, runs unconditionally so both correctly show silence
    // while Grains is off (gran_l already zeroed above in that case).
    for(size_t i = 0; i < size; i++)
    {
        g_granular_scope_l[g_granular_scope_write_pos] = gran_l[i];
        g_granular_scope_write_pos
            = (g_granular_scope_write_pos + 1) % kGranularScopeSamples;
        UpdateLevelMeter(g_grains_level, g_grains_clip_hold, gran_l[i], sample_rate);
    }

    // Skipped entirely when disabled from Global:Dexed -- same real
    // CPU-saving pattern as ui.IsGranularEnabled()/IsLooperEnabled()
    // above (a plain "don't call Process() at all", not just a mute).
    // Computed here, before the loop-layer block below, so dexed_l/r
    // are already valid when mixed_in is built from them.
    if(ui.IsDexedEnabled())
    {
        dexed.Process(size, dexed_l, dexed_r, reverb_send_l, reverb_send_r, delay_send_l,
                      delay_send_r);
    }
    else
    {
        for(size_t i = 0; i < size; i++)
        {
            dexed_l[i] = 0.f;
            dexed_r[i] = 0.f;
        }
    }

    // Same for Dexed's own oscilloscope pages (see g_dexed_scope_l's own
    // comment above) -- captured after the enabled/disabled branch above
    // either way, so the scope just shows silence while Dexed's off.
    // Also feeds Screen::Mixer's own Dexed level/clip meter, same signal.
    for(size_t i = 0; i < size; i++)
    {
        g_dexed_scope_l[g_dexed_scope_write_pos] = dexed_l[i];
        g_dexed_scope_write_pos = (g_dexed_scope_write_pos + 1) % kDexedScopeSamples;
        UpdateLevelMeter(g_dexed_level, g_dexed_clip_hold, dexed_l[i], sample_rate);
    }

    // Skipped entirely when disabled from Global:Ensemble -- same real
    // CPU-saving pattern as ui.IsDexedEnabled() above.
    if(ui.IsEnsembleEnabled())
    {
        ensemble.Process(size, ensemble_l, ensemble_r, reverb_send_l, reverb_send_r,
                          delay_send_l, delay_send_r);
    }
    else
    {
        for(size_t i = 0; i < size; i++)
        {
            ensemble_l[i] = 0.f;
            ensemble_r[i] = 0.f;
        }
    }

    // Same for Ensemble's own oscilloscope pages (see g_ensemble_scope_l's
    // own comment above) -- captured after the enabled/disabled branch
    // above either way, so the scope just shows silence while Ensemble's
    // off.
    for(size_t i = 0; i < size; i++)
    {
        g_ensemble_scope_l[g_ensemble_scope_write_pos] = ensemble_l[i];
        g_ensemble_scope_write_pos = (g_ensemble_scope_write_pos + 1) % kEnsembleScopeSamples;
    }

    // Skipped entirely when disabled from Global:SampleSynth -- same
    // real CPU-saving pattern as ui.IsEnsembleEnabled() above.
    if(ui.IsSampleSynthEnabled())
    {
        sample_synth.Process(size, samp_l, samp_r, reverb_send_l, reverb_send_r, delay_send_l,
                              delay_send_r);
    }
    else
    {
        for(size_t i = 0; i < size; i++)
        {
            samp_l[i] = 0.f;
            samp_r[i] = 0.f;
        }
    }

    // Same for SampleSynth's own oscilloscope pages -- captured after
    // the enabled/disabled branch above either way, so the scope just
    // shows silence while SampleSynth's off.
    for(size_t i = 0; i < size; i++)
    {
        g_sample_synth_scope_l[g_sample_synth_scope_write_pos] = samp_l[i];
        g_sample_synth_scope_write_pos
            = (g_sample_synth_scope_write_pos + 1) % kSampleSynthScopeSamples;
    }

    // SampleSynth's own Direct Record capture -- same shape as Grains'
    // own g_granular_capturing block below, writing live input plus
    // every OTHER generated-audio engine (Dexed/Grains/Ensemble, not
    // SampleSynth's own samp_l/r -- that would be a self-capture
    // feedback loop, same reasoning as Grains' own capture excluding
    // gran_l/r).
    if(g_sample_synth_capturing)
    {
        for(size_t i = 0; i < size; i++)
        {
            if(g_sample_synth_capture_write_pos >= kSampleSynthCaptureSamples)
            {
                g_sample_synth_capturing = false;
                break;
            }
            g_sample_synth_capture_l[g_sample_synth_capture_write_pos]
                = in[0][i] + dexed_l[i] + gran_l[i] + ensemble_l[i];
            g_sample_synth_capture_r[g_sample_synth_capture_write_pos]
                = in[1][i] + dexed_r[i] + gran_r[i] + ensemble_r[i];
            g_sample_synth_capture_write_pos++;
        }
    }

    // Grains' Direct Record capture -- see g_granular_capturing's own
    // comment above. Writes live input PLUS Dexed's own generated audio
    // (not Grains' own gran_l/r -- that would be a self-capture feedback
    // loop, capturing Grains' output back into the very buffer it's
    // about to grain from) -- same "internally-generated audio should be
    // capturable, not just a physical patch cable" reasoning as
    // mixed_in below, just for Grains' own capture buffer instead of a
    // loop layer.
    if(g_granular_capturing)
    {
        for(size_t i = 0; i < size; i++)
        {
            if(g_granular_capture_write_pos >= kGranularCaptureSamples)
            {
                g_granular_capturing = false;
                break;
            }
            g_granular_capture_l[g_granular_capture_write_pos]
                = in[0][i] + dexed_l[i] + ensemble_l[i] + samp_l[i];
            g_granular_capture_r[g_granular_capture_write_pos]
                = in[1][i] + dexed_r[i] + ensemble_r[i] + samp_r[i];
            g_granular_capture_write_pos++;
        }
    }

    // Recording input is live input PLUS Grains/Dexed's own generated
    // audio, summed -- so playing either instrument while a layer is
    // actively recording captures it together with the live input, same
    // "always summed, no toggle" decision this project already made
    // before (Pad/Fm's own equivalent construct, since removed along
    // with those instruments): a live+synth mix is what someone pressing
    // record while playing a MIDI keyboard actually wants.
    static float mixed_in_l[256];
    static float mixed_in_r[256];
    for(size_t i = 0; i < size; i++)
    {
        mixed_in_l[i] = in[0][i] + gran_l[i] + dexed_l[i] + ensemble_l[i] + samp_l[i];
        mixed_in_r[i] = in[1][i] + gran_r[i] + dexed_r[i] + ensemble_r[i] + samp_r[i];
    }
    const float* const       mixed_ptr_arr[2] = {mixed_in_l, mixed_in_r};
    AudioHandle::InputBuffer mixed_in         = mixed_ptr_arr;

    // Each layer mixes its own dry signal directly into out[], and adds
    // its Reverb Send contribution into reverb_send_l/r (see above --
    // the actual shared ReverbSc runs once per sample further down).
    //
    // Skipped entirely when disabled from Global:Looper -- a real stop,
    // not TogglePauseAll()'s own phase-locked pause: no layer's Process()
    // runs at all (no recording, no playback, no reverb-send
    // contribution), same "skip the call outright" CPU-saving pattern as
    // ui.IsGranularEnabled() above.
    if(looper_enabled)
    {
        float* reverb_send_ptrs[2] = {reverb_send_l, reverb_send_r};
        float* delay_send_ptrs[2]  = {delay_send_l, delay_send_r};
        for(int L = 0; L < kNumLayers; L++)
        {
            LayerState st = layers[L].GetState();
            bool       is_recording_ish = st == LayerState::Recording
                                           || st == LayerState::Overdubbing
                                           || st == LayerState::ArmedCountIn
                                           || st == LayerState::ArmedOverdubCountIn;
            AudioHandle::InputBuffer layer_in
                = is_recording_ish ? mixed_in : in;
            layers[L].Process(layer_in, out, reverb_send_ptrs, delay_send_ptrs, size, ticks,
                               tempo, project_speed);
        }
    }

    // Grains' own dry signal into the master mix -- unconditional, same
    // position (before the shared reverb Process() call and the master
    // filter) so it applies equally alongside the loops. (Disabled above
    // already zeroed gran_l/r, so this add is a harmless no-op rather
    // than needing its own separate mute.)
    for(size_t i = 0; i < size; i++)
    {
        out[0][i] += gran_l[i] + dexed_l[i] + ensemble_l[i] + samp_l[i];
        out[1][i] += gran_r[i] + dexed_r[i] + ensemble_r[i] + samp_r[i];
    }

    const float mv           = ui.GetMasterVolume();
    const bool  byp          = ui.IsBypassed();
    const float bypass_gain  = ui.GetBypassGain();
    const float bypass_reverb_send01 = ui.GetBypassReverbSend01();
    const float bypass_delay_send01  = ui.GetBypassDelaySend01();
    // Screen::Mixer's own Bypass channel -- multiplies on top of
    // bypass_gain above (see Ui::GetBypassMixVolume01()'s own comment for
    // why these are two separate controls), plus a genuine Pan, applied
    // to both the dry monitor mix and its reverb send below (same "pan
    // affects the send too" treatment LooperLayer::Process() already
    // uses for every loop layer).
    const float bypass_mix_volume = ui.GetBypassMixVolume();
    const float bypass_pan01      = ui.GetBypassPan01();
    const float bypass_pan_l      = 1.f - bypass_pan01;
    const float bypass_pan_r      = bypass_pan01;
    const FilterMode mfilt_mode = ui.GetMasterFilterMode();

    // Block-rate controls (matches LooperLayer's own per-layer filter
    // curve -- kFilterMinHz/kFilterMaxHz in looper_layer.h -- so the
    // master filter feels consistent with the per-layer ones).
    float mfilt_cutoff
        = kFilterMinHz * powf(kFilterMaxHz / kFilterMinHz, ui.GetMasterFilterCutoff01());
    float nyquist_guard = hw.AudioSampleRate() / 3.f - 1.f;
    mfilt_cutoff = mfilt_cutoff < kFilterMinHz ? kFilterMinHz
                   : mfilt_cutoff > nyquist_guard ? nyquist_guard
                                                   : mfilt_cutoff;
    float mfilt_res = ui.GetMasterFilterResonance01() * 0.9f;
    fx_master_filter_l.SetFreq(mfilt_cutoff);
    fx_master_filter_l.SetRes(mfilt_res);
    fx_master_filter_r.SetFreq(mfilt_cutoff);
    fx_master_filter_r.SetRes(mfilt_res);

    // Shared reverb's Size/decay -- one Global:Reverb setting (see
    // Ui::GetReverbSize01()) applied to the single shared instance,
    // replacing what used to be an independent SetFeedback() per layer.
    // "Size" is the direct continuation of this same knob/field; it maps
    // straight onto the Dattorro engine's own Decay parameter.
    fx_reverb_shared.SetDecay01(ui.GetReverbSize01());
    // The other 6 DattorroReverb parameters -- Global:Reverb Tone/
    // Diffusion (see Ui::GetReverbPreDelay01() and friends).
    fx_reverb_shared.SetPreDelay01(ui.GetReverbPreDelay01());
    fx_reverb_shared.SetPreFilter01(ui.GetReverbPreFilter01());
    fx_reverb_shared.SetInputDiffusion1_01(ui.GetReverbInputDiffusion1_01());
    fx_reverb_shared.SetInputDiffusion2_01(ui.GetReverbInputDiffusion2_01());
    fx_reverb_shared.SetDecayDiffusion01(ui.GetReverbDecayDiffusion01());
    fx_reverb_shared.SetDamping01(ui.GetReverbDamping01());

    // Shared delay's settings -- same "Global setting drives the single
    // shared instance" pattern as the reverb above. See SharedDelay for
    // the Time curve, Sync, Ping-Pong and Tone.
    fx_delay.Configure(ui.GetDelaySettings(), tempo.GetBpm(), hw.AudioSampleRate());
    // How much of the delay's output also feeds the reverb (Global:Dly-Rev)
    // -- so echoes sit in the same space as the dry sound.
    const float delay_to_reverb01 = ui.GetDelayToReverb01();

    // Both busses otherwise run their full fixed per-sample cost
    // unconditionally regardless of input level or decay/feedback
    // settings (confirmed on hardware: Reverb Size/Decay only scales a
    // feedback coefficient, never changes how much work Process() does)
    // -- a real, measured chunk of CPU even with every engine disabled
    // and nothing feeding either bus. Size/Time at their own lowest
    // reachable knob position now skips the real algorithm entirely
    // instead of running it on near-silent input. Known trade-off for
    // delay specifically: dropping Time to 0 while a long Feedback tail
    // is still ringing freezes that tail rather than fading it out --
    // accepted deliberately, same spirit as Reverb Size already losing
    // its own initial diffusion "splash" at Decay 0 (inaudible there
    // since zero decay already kills the tail almost immediately).
    // A real pot (Ui::ApplyKnobs()'s raw_k1/raw_k2) almost never settles
    // on an exact 0.0f at its physical end-stop -- ADC noise, the pot's
    // own tolerance, and the existing drift deadband all mean the real
    // value can easily sit at e.g. 0.002 while still rounding to the
    // "0%" the screen shows (that rounding is `(int)(v*100+0.5)`, which
    // is 0 for any v < 0.005). A strict `<= 0.f` check would almost
    // never fire in practice, bypassing nothing despite the screen
    // reading 0% -- matching that same display-rounding threshold here
    // instead keeps "shows 0% on screen" and "is actually bypassed"
    // perfectly in sync.
    const bool reverb_bypassed = ui.GetReverbSize01() < 0.005f;
    const bool delay_bypassed  = SharedDelay::IsOff(ui.GetDelaySettings().time01);

    for(size_t i = 0; i < size; i++)
    {
        // Bypass's own Send into the shared reverb bus -- independent of
        // every layer's own Send, same bus though. Must happen BEFORE
        // the Process() call right below, which is what actually
        // consumes reverb_send_l/r for this sample. Same bypass_gain/
        // mix-volume-scaled, L+R-summed-to-mono treatment as the dry
        // bypass mix further down (see its own comment for why mono),
        // now also panned the same way -- same "pan affects the send
        // too" treatment LooperLayer::Process() uses for every layer.
        if(byp && bypass_reverb_send01 > 0.f)
        {
            float byp_mono_base = (in[0][i] + in[1][i]) * bypass_gain * bypass_mix_volume;
            reverb_send_l[i] += byp_mono_base * bypass_pan_l * bypass_reverb_send01;
            reverb_send_r[i] += byp_mono_base * bypass_pan_r * bypass_reverb_send01;
        }
        // Same for Bypass's own Delay Send (Global:Bypass, B2) -- before
        // the delay reads delay_send_l/r for this sample further down.
        if(byp && bypass_delay_send01 > 0.f)
        {
            float byp_mono_base = (in[0][i] + in[1][i]) * bypass_gain * bypass_mix_volume;
            delay_send_l[i] += byp_mono_base * bypass_pan_l * bypass_delay_send01;
            delay_send_r[i] += byp_mono_base * bypass_pan_r * bypass_delay_send01;
        }

        // Shared delay first, so its echoes can also feed the reverb
        // below (Global:Dly-Rev). Skipped entirely at Time 0 -- see
        // delay_bypassed's own comment above.
        if(!delay_bypassed)
        {
            float delay_wet_l, delay_wet_r;
            fx_delay.Process(delay_send_l[i], delay_send_r[i], &delay_wet_l, &delay_wet_r);
            out[0][i] += delay_wet_l;
            out[1][i] += delay_wet_r;
            reverb_send_l[i] += delay_wet_l * delay_to_reverb01;
            reverb_send_r[i] += delay_wet_r * delay_to_reverb01;
        }

        // Shared reverb: process the summed sends once per sample and
        // mix the wet result into out[] -- BEFORE bypass/master filter/
        // click, same position this used to be added in when it ran
        // per-layer inside LooperLayer::Process() itself. Skipped
        // entirely at Size 0 -- see reverb_bypassed's own comment above.
        if(!reverb_bypassed)
        {
            float rev_wet_l, rev_wet_r;
            fx_reverb_shared.Process(reverb_send_l[i], reverb_send_r[i], &rev_wet_l,
                                       &rev_wet_r);
            out[0][i] += rev_wet_l;
            out[1][i] += rev_wet_r;
        }

        if(byp)
        {
            // Digital "wet monitor" mix -- the original's analog bypass
            // relay had no equivalent DSP path to hook into on the Pod,
            // so this mixes a dry copy of the live input straight into
            // the output bus, same idea (hear your loops AND your live
            // input) implemented in software. See DESIGN.md. Scaled by
            // the selected layer's input gain so a quiet source (e.g. a
            // guitar with no preamp) is actually audible here too, not
            // just once it's recorded.
            //
            // Summed to mono (both input channels added together, sent
            // to both outputs) rather than kept as independent L/R --
            // the Pod has one physical input jack (a stereo TRS), and a
            // plain mono guitar cable plugged into it only excites ONE
            // of the two ADC channels (the other gets shorted to ground
            // by the missing ring contact), so a straight per-channel
            // passthrough left that signal audible on only one output
            // side. This only affects the bypass *monitor* -- recording
            // still captures each ADC channel independently (see
            // LooperLayer::Process()'s mic_in/guitar_in), so a genuinely
            // stereo source still records in true stereo.
            //
            // Also scaled by Screen::Mixer's own Bypass Volume and panned
            // (bypass_mix_volume/bypass_pan_l/r above) -- panning this
            // mono signal spreads it across the stereo field same as any
            // other mixer channel, same linear law as everything else.
            float byp_mono = (in[0][i] + in[1][i]) * bypass_gain * bypass_mix_volume;
            out[0][i] += byp_mono * bypass_pan_l;
            out[1][i] += byp_mono * bypass_pan_r;
            UpdateLevelMeter(g_bypass_level, g_bypass_clip_hold, byp_mono, sample_rate);
        }
        else
        {
            // No live bypass signal at all while off -- decay the meter
            // toward silence instead of leaving it holding a stale
            // reading, same release rate UpdateLevelMeter() uses (not a
            // separate, much-faster ad hoc decay).
            g_bypass_level -= g_bypass_level * (1.f / (kMeterReleaseSeconds * sample_rate));
            if(g_bypass_clip_hold > 0.f)
                g_bypass_clip_hold -= 1.f / sample_rate;
        }

        // Master filter -- applied to the full mix (all layers, their
        // reverb, and the bypass monitor) but deliberately AFTER this
        // point so the metronome click below stays unfiltered and
        // stays clearly audible regardless of the filter setting.
        if(mfilt_mode != FilterMode::Off)
        {
            fx_master_filter_l.Process(out[0][i]);
            fx_master_filter_r.Process(out[1][i]);
            switch(mfilt_mode)
            {
                case FilterMode::LowPass:
                    out[0][i] = fx_master_filter_l.Low();
                    out[1][i] = fx_master_filter_r.Low();
                    break;
                case FilterMode::HighPass:
                    out[0][i] = fx_master_filter_l.High();
                    out[1][i] = fx_master_filter_r.High();
                    break;
                case FilterMode::BandPass:
                    out[0][i] = fx_master_filter_l.Band();
                    out[1][i] = fx_master_filter_r.Band();
                    break;
                default: break;
            }
        }

        out[0][i] = (out[0][i] + click[i]) * mv;
        out[1][i] = (out[1][i] + click[i]) * mv;

        // Screen::Mixer's own oscilloscope -- the actual final signal,
        // captured last, same "write every block, wrap" pattern as
        // g_pad_scope_l/g_granular_scope_l above. Also feeds the Master
        // channel's own level/clip meter on the same page.
        g_master_scope_l[g_master_scope_write_pos] = out[0][i];
        g_master_scope_write_pos = (g_master_scope_write_pos + 1) % kMasterScopeSamples;
        UpdateLevelMeter(g_master_level, g_master_clip_hold, out[0][i], sample_rate);
    }

    FinishCpuMeterBlock(size, sample_rate);
}

// Linker-provided symbols from STM32H750IB_qspi_custom.lds -- .itcm_text
// has a load address (LMA) in QSPIFLASH and a run address (VMA) in
// ITCMRAM, same relationship .data already has between QSPIFLASH and
// SRAM. The stock startup code's own copy-down loop only knows about
// .data's symbols, not this new section, so nothing copies these bytes
// into ITCM without this -- ITCM has no power-on contents of its own
// (same "not zero/not anything in particular at boot" issue this
// project already hit with .sdram_bss, except this is executable code,
// not data, so the CPU would execute whatever garbage was already
// sitting there instead of just misbehaving numerically).
extern uint8_t _sitcm_text[];
extern uint8_t _eitcm_text[];
extern uint8_t _siitcm_text[];

int main(void)
{
    // Must run before anything DSY_ITCM_TEXT-tagged is ever called --
    // in practice that's hw.StartAudio(AudioCallback) further down, but
    // this sits right at the top of main() for the widest possible
    // safety margin.
    memcpy(_sitcm_text, _siitcm_text, (size_t)(_eitcm_text - _sitcm_text));

    // Flush-to-zero: without this, the FPU can fall back to slow
    // microcoded handling for denormalized floats -- and a feedback
    // network decaying toward silence (the shared reverb's tail, the
    // shared delay's repeats, every layer's own fade-outs) is exactly
    // the signal shape that produces them, on every single use. No
    // audible cost: denormals are values near 1e-38 and smaller.
    // __set_FPSCR takes effect immediately for the foreground context;
    // FPU->FPDSCR is the DEFAULT status word a lazily-saved FPU context
    // gets restored to after an interrupt, so that path doesn't quietly
    // come back without it. Same "as early as possible" placement as
    // the ITCM copy just above -- before any floating-point code (which
    // is effectively everything past this point) ever runs.
    __set_FPSCR(__get_FPSCR() | FPU_FPDSCR_FZ_Msk);
    FPU->FPDSCR |= FPU_FPDSCR_FZ_Msk;

    hw.Init();

    // hw.Init() -> InitMidi() claims D13 as MIDI UART TX via
    // MidiUartHandler::Config's default-constructed transport_config --
    // the exact same physical pin as the encoder's click button (see
    // daisy_pod.cpp's ENC_CLICK_PIN). The Pod's actual MIDI IN jack only
    // ever wires up D14/RX per Electrosmith's own pinout diagram, so that
    // TX claim is never usable here regardless -- re-init with TX
    // disabled (Pin(PORTX, 0), the standard "don't claim this pin"
    // sentinel) and re-init the encoder afterward to reclaim D13, rather
    // than relying on TX simply never being driven.
    {
        MidiUartHandler::Config midi_config;
        midi_config.transport_config.tx = Pin(PORTX, 0);
        hw.midi.Init(midi_config);
        hw.encoder.Init(seed::D26, seed::D25, seed::D13);
    }
    hw.midi.StartReceive();

    // 64, not 48 -- exactly matches msfa's own fixed 64-sample
    // rendering quantum (_N_ in msfa/synth.h), so DexedSynth::Process()'s
    // staging buffer (stage_f_/stage_pos_) never carries an irregular
    // leftover between calls the way 48 did (LCM(48,64)=192 -- a messy
    // relationship). compute()'s own real cost is unchanged either way
    // (750 calls/sec, tied to the quantum, not the outer block), traded
    // for ~0.33ms more latency per block (1.333ms vs 1ms) -- but the
    // real-world saving turned out bigger than that bookkeeping-cleanup
    // framing alone predicted: a controlled same-content A/B (identical
    // performance/sounds, only this value changed) measured 94% peak
    // CPU at 48 vs 84% at 64, confirmed on hardware, not just fewer ISR
    // entries on paper.
    hw.SetAudioBlockSize(64);
    // SetAudioBlockSize() above just called SetHidUpdateRates()
    // internally, which reconfigures knob1/knob2's own smoothing-filter
    // time constant from AudioCallbackRate() (sample_rate/block_size --
    // now 750Hz). That's the WRONG rate for this project: knobs are
    // actually always processed at a fixed, real 1kHz from
    // ControlTimerCallback() (TIM5, see its own comment below),
    // completely independent of the audio block size -- the two numbers
    // only matched before by coincidence (1000Hz block-implied rate ==
    // 1000Hz real poll rate at the old block size of 48). Left alone,
    // this would silently make every knob read slightly less smoothed/
    // twitchier than intended, for a reason that has nothing to do with
    // the Dexed-alignment goal above -- re-assert the real rate
    // explicitly instead of trusting what the library just computed.
    hw.knob1.SetSampleRate(1000.f);
    hw.knob2.SetSampleRate(1000.f);

    tempo.Init(hw.AudioSampleRate());

    // See audio_engine.h's own comment on g_cpu_avg_load/g_cpu_peak_load.
    // Default 1Hz smoothing cutoff (CpuLoadMeter's own default) is fine
    // for both -- the peak instance's own periodic Reset() (see
    // kCpuPeakResetSeconds in AudioCallback()) is what actually keeps
    // its reading current, not a faster smoothing filter.
    // hw.AudioBlockSize() (the real, currently-configured value), not a
    // literal -- CpuLoadMeter::Init() needs the actual block size to
    // know how much real time counts as "100% load" per block
    // (secPerBlock = blockSize/sampleRate internally); a stale literal
    // here silently miscalibrates every reading by whatever ratio the
    // real block size has since changed to (this exact bug, caught
    // after the 48->64 block size change: both calls still said `48`,
    // inflating every reading by 64/48 =~1.33x -- a measurement error,
    // not an actual CPU increase).
    g_cpu_meter_avg.Init(hw.AudioSampleRate(), hw.AudioBlockSize());
    g_cpu_meter_peak.Init(hw.AudioSampleRate(), hw.AudioBlockSize());

    for(int i = 0; i < kNumLayers; i++)
        layers[i].Init(buffer_l[i], buffer_r[i], kBuffSize, hw.AudioSampleRate(),
                       &fx_phaser[i], g_undo_backup_l, g_undo_backup_r);

    fx_master_filter_l.Init(hw.AudioSampleRate());
    fx_master_filter_r.Init(hw.AudioSampleRate());

    granular.Init(hw.AudioSampleRate());
    granular.SetSourceShadow(g_granular_shadow, kGranularCaptureSamples);
    dexed.Init(hw.AudioSampleRate());
    ensemble.Init(hw.AudioSampleRate());
    sample_synth.Init(hw.AudioSampleRate());

    // The Stage-1 diagnostic overrides that used to force max Fill/zero
    // Gap/an always-on Scan sweep (needed back when there were no real
    // knobs to test with) are gone now that the Grain page is fully
    // built -- the class's own defaults (Fill=2/3, Gap=0.1, Scan off)
    // apply at boot instead, same as every other engine's own hardcoded
    // defaults. Also meaningfully cheaper at rest: max Fill was forcing
    // the densest, most CPU-expensive grain scheduling as the permanent
    // starting state every boot, not just as something the Fill knob
    // could reach if asked for.

    // No manual zero needed here any more -- fx_reverb_shared AND
    // fx_reverb_storage (see its own declaration comment: moved out of
    // SDRAM into plain SRAM to test whether SDRAM's own per-access
    // latency was the reverb's dominant CPU cost) are both plain .bss
    // now, already zero at boot. Contrast fx_delay's own Init()
    // just below -- those are still real .sdram_bss objects, which
    // (per libDaisy's own sdram.h, confirmed against the linker
    // script/startup code) start out holding raw leftover SDRAM
    // contents, not zero, since only ordinary .bss gets zero-filled at
    // boot.
    fx_reverb_shared.Init(hw.AudioSampleRate(), fx_reverb_storage);
    // Init() itself seeds every parameter (PreDelay, PreFilter, both Input
    // Diffusions, DecayDiffusion, Damping) to the original library's own
    // tuned defaults -- nothing else to set here. Decay itself (the one
    // parameter that WAS already user-facing as "Size") is applied every
    // block in AudioCallback() from Ui::GetReverbSize01() (see there), same
    // live-update pattern as the master filter's cutoff/res just above.

    // SDRAM isn't zeroed at boot -- Bus::Init() clears all of it first.
    fx_delay.Init();

    // I2C1 defaults already target the Pod's header pins (D11=SCL,
    // D12=SDA) and address 0x3C -- most cheap SSD1306/SSD1309 modules
    // need no further configuration. If your module replies on 0x3D
    // instead, uncomment the line below.
    MyDisplay::Config disp_cfg;
    // disp_cfg.driver_config.transport_config.i2c_address = 0x3D;

    // libDaisy's SSD130x transport defaults to 1MHz I2C (Fast Mode Plus).
    // That's aggressive for typical cheap OLED breakout pull-ups/header
    // wiring, and this driver has no bus-recovery: SendData() blocks
    // byte-by-byte with a 1s timeout each, so one glitched byte in a
    // 1024-byte frame can stall the *entire* main loop -- which is also
    // where the encoder/buttons/pot get read -- for up to ~17 minutes.
    // 400kHz is far more forgiving and still plenty fast for this display.
    disp_cfg.driver_config.transport_config.i2c_config.speed
        = daisy::I2CHandle::Config::Speed::I2C_400KHZ;
    display.Init(disp_cfg);

    PerformanceStore::Init(); // mounts the SD card if one is present

    ui.Init(&hw, &display, &tempo, layers, kNumLayers, &granular, g_granular_scope_l,
            kGranularScopeSamples, g_granular_capture_l, g_granular_capture_r,
            kGranularCaptureSamples, &g_granular_capturing, &g_granular_capture_write_pos,
            g_master_scope_l, kMasterScopeSamples, g_dexed_scope_l, kDexedScopeSamples,
            &dexed, &ensemble, g_ensemble_scope_l, kEnsembleScopeSamples,
            &sample_synth, g_sample_synth_scope_l, kSampleSynthScopeSamples,
            g_sample_synth_arena_l, g_sample_synth_arena_r, kSampleSynthArenaSamples,
            g_sample_synth_capture_l, g_sample_synth_capture_r, kSampleSynthCaptureSamples,
            &g_sample_synth_capturing, &g_sample_synth_capture_write_pos, g_undo_backup_l,
            g_undo_backup_r, &g_undo_owner_layer,
            &g_undo_backup_len);
    ui.ApplyStartupDefaults(); // no-op if nothing's been saved yet (see PerformanceStore::LoadPrefs())

    // Hardware PWM setup for LED1-Green/LED1-Blue/LED2-Red -- see
    // led_pwm_tim3's own comment above for which pins/why only these 3.
    // Must run after hw.Init() (already happened, above) since
    // DaisyPod::InitLeds() configures D17/D18/D19 as plain GPIO outputs;
    // this reconfigures those same 3 pins to the timer's own alternate
    // function, superseding that -- the corresponding daisy::Led
    // sub-objects inside pod_->led1/led2 are simply never Update()'d
    // again (Ui::UpdateLeds() no longer calls pod_->UpdateLeds() at all).
    {
        daisy::PWMHandle::Config pwm_cfg;
        pwm_cfg.periph = daisy::PWMHandle::Config::Peripheral::TIM_3;
        pwm_cfg.period = 0xff; // 8-bit duty resolution, plenty for a flat on/off level
        led_pwm_tim3.Init(pwm_cfg);

        // All 3 need Polarity::LOW: this Pod's RGB LEDs are wired
        // active-low (see DaisyPod::InitLeds()'s own led1/led2.Init(...,
        // /*invert=*/true) call) -- PWMHandle's own default is
        // Polarity::HIGH ("pin high during the active/on fraction"),
        // which on an active-low LED drives it ON during the fraction
        // meant to be OFF and vice versa. Confirmed as a real bug on
        // hardware: at the default polarity, Set(0.0) ("off") held the
        // pin low 100% of the time, which an active-low LED reads as
        // fully ON, permanently.
        //
        // Channel 1 (LED1-Green, D19/PA6) and Channel 4 (LED2-Red,
        // D17/PB1) both default to exactly the pin we want -- only the
        // polarity needs overriding, not the pin.
        daisy::PWMHandle::Channel::Config ch1_cfg;
        ch1_cfg.polarity = daisy::PWMHandle::Channel::Config::Polarity::LOW;
        led_pwm_tim3.Channel1().Init(ch1_cfg);

        daisy::PWMHandle::Channel::Config ch4_cfg;
        ch4_cfg.polarity = daisy::PWMHandle::Channel::Config::Polarity::LOW;
        led_pwm_tim3.Channel4().Init(ch4_cfg);

        // Channel 2 defaults to PC7 (the Seed's own onboard LED) --
        // must override the pin to PA7/D18 (LED1-Blue) too.
        daisy::PWMHandle::Channel::Config ch2_cfg;
        ch2_cfg.pin      = seed::D18;
        ch2_cfg.polarity = daisy::PWMHandle::Channel::Config::Polarity::LOW;
        led_pwm_tim3.Channel2().Init(ch2_cfg);

        ui.SetLedPwmChannels(&led_pwm_tim3.Channel1(), &led_pwm_tim3.Channel2(),
                              &led_pwm_tim3.Channel4());
    }

    hw.StartAdc();
    hw.StartAudio(AudioCallback);

    // Drive control polling from TIM5 at 1kHz -- matches Encoder/Switch's
    // own internal 1kHz debounce rate cap, so nothing is gained by going
    // faster. TIM2 is off-limits (used internally by System for
    // sub-millisecond timing); TIM5 is otherwise unused here. See
    // ControlTimerCallback() above and ui.h's UiControlEvents comment for
    // why this can't just be a call in the while(1) loop below.
    TimerHandle::Config tim_cfg;
    tim_cfg.periph     = TimerHandle::Config::Peripheral::TIM_5;
    tim_cfg.enable_irq = true;
    tim_cfg.period     = System::GetPClk2Freq() / 1000;
    control_timer.Init(tim_cfg);
    control_timer.SetCallback(ControlTimerCallback);
    control_timer.Start();

    // Main loop's own "last seen" checkpoints for the ISR's monotonic
    // counters, used to build each iteration's UiControlEvents delta.
    int32_t  last_encoder_pos         = 0;
    uint32_t last_encoder_click_falls = 0;
    uint32_t last_btn1_releases       = 0;
    uint32_t last_btn2_releases       = 0;

    for(;;)
    {
        UiControlEvents events;
        events.encoder_delta      = g_encoder_pos - last_encoder_pos;
        events.encoder_click_fell = g_encoder_click_falls != last_encoder_click_falls;
        events.btn1_released      = g_btn1_releases != last_btn1_releases;
        events.btn2_released      = g_btn2_releases != last_btn2_releases;
        events.btn1_held_ms       = g_btn1_held_ms;
        events.btn2_held_ms       = g_btn2_held_ms;
        last_encoder_pos          = g_encoder_pos;
        last_encoder_click_falls  = g_encoder_click_falls;
        last_btn1_releases        = g_btn1_releases;
        last_btn2_releases        = g_btn2_releases;

        // MIDI is polled from ControlTimerCallback() (TIM5 ISR) now, not
        // here -- see its own comment for why.

        // Claim Undo-backup ownership -- the real copy already happened
        // inside LooperLayer::Process() itself, spread across the
        // overdub's own count-in (see ConsumeOverdubSnapshotRequest()'s
        // own comment for why: a main-loop-deferred bulk copy, like this
        // used to be, could race real overdub audio already landing in
        // the buffer). This is just cheap bookkeeping -- which layer's
        // backup is now valid -- safe to leave at the main loop's own
        // pace since it's no longer time-critical.
        for(int L = 0; L < kNumLayers; L++)
        {
            if(!layers[L].ConsumeOverdubSnapshotRequest())
                continue;
            g_undo_owner_layer = L;
            g_undo_backup_len  = layers[L].GetRecordedLength();
        }

        ui.Update(events);

        hw.DelayMs(1);
    }
}
