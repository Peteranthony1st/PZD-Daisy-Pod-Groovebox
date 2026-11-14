// The host renderer: the firmware's own Ui (ui.cpp and friends) running on
// the computer, driven by a script of what a player does, saving the
// screen whenever the script says. Every image is exactly what the Pod
// draws, because it's the Pod's own drawing code.
//
//   render <script> <out_dir>
//
// Script lines (# starts a comment):
//   wait <ms>          let time pass (the UI updates every 10 ms)
//   turn <steps>       turn the encoder (negative = anticlockwise)
//   click              click the encoder
//   hold enc|b1|b2 <ms>  hold a control, then let go
//   tap b1|b2          tap a button
//   knob 1|2 <0..1>    set a knob (moved there in small steps, as a hand would)
//   shot <name>        save the screen as <out_dir>/<name>.pbm
//
// The SD card is the folder in $PZD_HOST_SD (no card without it).
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include "daisy_pod.h"
#include "dev/oled_ssd130x.h"
#include "ui.h"
#include "audio_engine.h"
#include "profiler.h"
#include "granular_engine.h"
#include "dexed_synth.h"
#include "ensemble_synth.h"
#include "supersaw_synth.h"
#include "sample_synth.h"
#include "sequencer.h"
#include "arpeggiator.h"
#include "live_export.h"
#include "performance_store.h"

using namespace daisy;

uint32_t System::now_ms = 0;

// What main.cpp defines and the rest of the firmware reads.
volatile bool     g_audio_suspended = false;
volatile bool     g_seq_hold        = false;
volatile uint32_t g_loop_starts     = 0;
float g_grains_level = 0.f, g_dexed_level = 0.f, g_bypass_level = 0.f, g_master_level = 0.f;
float g_grains_clip_hold = 0.f, g_dexed_clip_hold = 0.f, g_bypass_clip_hold = 0.f,
      g_master_clip_hold = 0.f;
float g_cpu_avg_load = 0.f, g_cpu_peak_load = 0.f;
namespace Prof
{
float g_avg[kNumStages];
float g_peak[kNumStages];
}
volatile uint32_t g_seq_tick = 0, g_seq_loop_ticks = 0, g_seq_record_starts = 0;
volatile bool     g_seq_running = false;

constexpr size_t kBuffSize = 921600, kNumLayers = 4;
constexpr size_t kGranularCaptureSamples = 480000, kSampleSynthArenaSamples = 1600000,
                 kSampleSynthCaptureSamples = 288000, kScope = 1024;

static float   buffer_l[kNumLayers][kBuffSize], buffer_r[kNumLayers][kBuffSize];
static float   g_undo_backup_l[kBuffSize], g_undo_backup_r[kBuffSize];
static int     g_undo_owner_layer = -1;
static size_t  g_undo_backup_len  = 0;
static int16_t g_granular_shadow[kGranularCaptureSamples * 2];
static float   g_granular_capture_l[kGranularCaptureSamples], g_granular_capture_r[kGranularCaptureSamples];
static volatile bool   g_granular_capturing        = false;
static volatile size_t g_granular_capture_write_pos = 0;
alignas(4) static int16_t g_sample_synth_arena[4][kSampleSynthArenaSamples];
static float g_sample_synth_capture_l[kSampleSynthCaptureSamples],
    g_sample_synth_capture_r[kSampleSynthCaptureSamples];
static volatile bool   g_sample_synth_capturing        = false;
static volatile size_t g_sample_synth_capture_write_pos = 0;
static float g_scope[6][kScope];
static MidiSequencer::Event
    g_seq_patterns[MidiSequencer::kTracks * MidiSequencer::kPatterns * MidiSequencer::kMaxEvents];

static DaisyPod       hw;
static HostDisplay    display;
static TempoClock     tempo;
static LooperLayer    layers[kNumLayers];
static GranularEngine granular;
static DexedBus       dexed_bus;
static DexedSynth     dexed, dexed_b;
static EnsembleSynth  ensemble, organ;
static SupersawSynth  supersaw;
static SampleSynth    sample_synth, sample_drum;
static MidiSequencer  sequencer;
static Arpeggiator    g_arp;
static LiveExport     g_live_export;
static Ui             ui;

static void SeqNote(int, int, uint8_t, uint8_t) {}
static void ArpNote(int, uint8_t, uint8_t) {}

static const float kSr = 48000.f;

static void Init()
{
    tempo.Init(kSr);
    for(size_t i = 0; i < kNumLayers; i++)
        layers[i].Init(buffer_l[i], buffer_r[i], kBuffSize, kSr, g_undo_backup_l, g_undo_backup_r);
    granular.Init(kSr);
    granular.SetSourceShadow(g_granular_shadow, kGranularCaptureSamples);
    dexed_bus.Init(kSr);
    dexed.SetBus(&dexed_bus);
    dexed_b.SetBus(&dexed_bus);
    dexed.Init(kSr);
    dexed_b.Init(kSr);
    ensemble.Init(kSr);
    organ.SetOrgan(true);
    organ.Init(kSr);
    supersaw.Init(kSr);
    sample_synth.Init(kSr);
    sample_drum.Init(kSr);
    sample_drum.SetVelSens01(SampleSynth::kDefaultDrumVelSens01);
    sequencer.Init(&SeqNote, g_seq_patterns);
    g_arp.Init(&ArpNote);

    Ui::DrawSplash(&display);
    PerformanceStore::Init();
    ui.Init(&hw, &display, &tempo, layers, kNumLayers, &granular, g_scope[0], kScope,
            g_granular_capture_l, g_granular_capture_r, kGranularCaptureSamples,
            &g_granular_capturing, &g_granular_capture_write_pos, g_scope[1], kScope, g_scope[2],
            kScope, &dexed, &dexed_b, &ensemble, &organ, g_scope[3], kScope, &sample_synth,
            &sample_drum, g_scope[4], kScope, g_scope[5], kScope, &g_sample_synth_arena[0][0],
            kSampleSynthArenaSamples, g_sample_synth_capture_l, g_sample_synth_capture_r,
            kSampleSynthCaptureSamples, &g_sample_synth_capturing,
            &g_sample_synth_capture_write_pos, g_undo_backup_l, g_undo_backup_r,
            &g_undo_owner_layer, &g_undo_backup_len);
    ui.SetSupersaw(&supersaw);
    ui.SetSequencer(&sequencer);
    ui.SetArpeggiator(&g_arp);
    g_live_export.seg[0]     = reinterpret_cast<int16_t*>(g_undo_backup_l);
    g_live_export.seg[1]     = reinterpret_cast<int16_t*>(g_undo_backup_r);
    g_live_export.seg_frames = kBuffSize;
    ui.SetLiveExport(&g_live_export);
    ui.ApplyStartupDefaults();
}

// One main-loop pass, 10 ms of time.
static void Step(UiControlEvents ev = UiControlEvents())
{
    System::now_ms += 10;
    ui.Update(ev);
}

static Switch& Control(const std::string& c)
{
    return c == "b1" ? hw.button1 : c == "b2" ? hw.button2 : hw.encoder.sw;
}

static void Release(const std::string& c, float held_ms)
{
    Control(c).pressed = false;
    UiControlEvents ev;
    if(c == "b1")
    {
        ev.btn1_released = true;
        ev.btn1_held_ms  = held_ms;
    }
    else if(c == "b2")
    {
        ev.btn2_released = true;
        ev.btn2_held_ms  = held_ms;
    }
    else
        ev.encoder_click_fell = true;
    Step(ev);
}

static void Hold(const std::string& c, int ms)
{
    Switch& s = Control(c);
    s.pressed = true;
    s.down_ms = System::now_ms;
    for(int t = 0; t < ms; t += 10)
        Step();
    Release(c, (float)ms);
}

static void Rest()
{
    for(int t = 0; t < 250; t += 10)
        Step();
}

static void Save(const std::string& path)
{
    FILE* f = fopen(path.c_str(), "w");
    if(!f)
    {
        perror(path.c_str());
        exit(1);
    }
    fprintf(f, "P1\n128 64\n");
    for(int y = 0; y < 64; y++)
    {
        for(int x = 0; x < 128; x++)
            fputc(display.px[y][x] ? '1' : '0', f);
        fputc('\n', f);
    }
    fclose(f);
}

int main(int argc, char** argv)
{
    if(argc < 3)
    {
        fprintf(stderr, "usage: render <script> <out_dir>\n");
        return 2;
    }
    FILE* in = fopen(argv[1], "r");
    if(!in)
    {
        perror(argv[1]);
        return 1;
    }
    Init();
    char line[256];
    int  n = 0;
    while(fgets(line, sizeof(line), in))
    {
        n++;
        char* hash = strchr(line, '#');
        if(hash)
            *hash = 0;
        char cmd[32] = {}, a[64] = {}, b[64] = {};
        if(sscanf(line, "%31s %63s %63s", cmd, a, b) < 1)
            continue;
        std::string c = cmd;
        if(c == "wait")
            for(int t = 0; t < atoi(a); t += 10)
                Step();
        else if(c == "turn")
        {
            // One detent at a time, as the encoder sends them.
            const int steps = atoi(a);
            for(int i = 0; i < abs(steps); i++)
            {
                UiControlEvents ev;
                ev.encoder_delta = steps > 0 ? 1 : -1;
                Step(ev);
                Step();
            }
        }
        // After each press, a moment before the next, as a hand leaves:
        // presses closer together than that aren't all seen.
        else if(c == "click")
        {
            Hold("enc", 60);
            Rest();
        }
        else if(c == "hold")
        {
            Hold(a, atoi(b));
            Rest();
        }
        else if(c == "tap")
        {
            Hold(a, 60);
            Rest();
        }
        else if(c == "knob")
        {
            const int   k    = atoi(a) == 2 ? 1 : 0;
            const float to   = (float)atof(b);
            float       from = hw.knob[k];
            for(int i = 1; i <= 20; i++)
            {
                hw.knob[k] = from + (to - from) * (float)i / 20.f;
                Step();
            }
        }
        else if(c == "shot")
        {
            // The UI redraws every 20th update: run 20, clock still.
            for(int i = 0; i < 20; i++)
                ui.Update(UiControlEvents());
            Save(std::string(argv[2]) + "/" + a + ".pbm");
        }
        else
        {
            fprintf(stderr, "%s:%d: what's \"%s\"?\n", argv[1], n, cmd);
            return 1;
        }
    }
    return 0;
}
