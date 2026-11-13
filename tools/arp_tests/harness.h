// Host model of main.cpp's arp/sequencer glue: the same routing and clock
// code, driven by a simulated audio callback (64-sample blocks at 48kHz)
// and a 2kHz control interrupt.
#pragma once
#include "arpeggiator.h"
#include "sequencer.h"
#include "tempo_clock.h"
#include <cstdio>
#include <vector>
#include <cmath>

struct NoteLog { double sample; int engine; int note; int vel; int src; uint64_t tick; }; // src: arp instance, 100 live direct, 200+track seq direct
struct Sim
{
    static constexpr int kSusEngines = 7;
    TempoClock tempo;
    MidiSequencer seq;
    Arpeggiator arp;
    std::vector<MidiSequencer::Event> store = std::vector<MidiSequencer::Event>(MidiSequencer::kTracks*MidiSequencer::kPatterns*MidiSequencer::kMaxEvents);
    // published by the audio callback
    uint32_t g_seq_tick=0, g_seq_loop_ticks=0, g_record_starts=0; bool g_seq_running=false;
    bool looper_enabled=true; bool suspended=false, seq_hold=false;
    // main.cpp's interrupt as it was (true: the sequencer and the arp clock
    // read the published clock separately), and how long the audio callback
    // may take to preempt it between the two (0 = never).
    bool separate_reads=false; double preempt_us=0; bool preempt_now=false;
    double sample=0;        // samples since start
    double us=0;            // time
    uint64_t timer_count=0; // GetTick()
    std::vector<NoteLog> log;
    int src=-1;              // who's sending the note being logged
    int src_sounding[300][7][128]={}; // per source
    int arp_on[Arpeggiator::kInstances]={}; int arp_overlap=0;
    int sounding[kSusEngines][128]={};
    bool g_arp_key[Arpeggiator::kInstances][128]={};
    int8_t g_arp_note_engine[Arpeggiator::kInstances][128]={};
    bool g_seq_arp_note[MidiSequencer::kTracks][128]={};
    // ArpTick statics
    uint64_t tick=0; uint32_t prev=0; bool was_running=false; float frac=0; uint64_t last_count=0; bool first=true;
    bool track_was_playing[8]={}; uint32_t track_pos[8]={}; bool seq_was_running=false;
};
extern Sim* S;
void EngineNoteOn(int e, uint8_t n, uint8_t v);
void EngineNoteOff(int e, uint8_t n);
void Init(float bpm, int bars);
void Run(double seconds);
void SetLooper(bool on);
void LiveKeyOn(int arp, int e, uint8_t n, uint8_t v);
void LiveKeyOff(int arp, int e, uint8_t n);
