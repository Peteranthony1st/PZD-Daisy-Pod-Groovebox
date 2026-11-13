#include "harness.h"
Sim* S=nullptr;
static const double kRate=48000.0, kTickFreq=240e6; // GetTickFreq
void EngineNoteOn(int e, uint8_t n, uint8_t v){ S->log.push_back({S->sample,e,n,v,S->src,S->tick}); S->sounding[e][n]++; if(S->src>=0) S->src_sounding[S->src][e][n]++; }
void EngineNoteOff(int e, uint8_t n){ S->log.push_back({S->sample,e,n,0,S->src,S->tick}); if(S->sounding[e][n]>0) S->sounding[e][n]--; if(S->src>=0 && S->src_sounding[S->src][e][n]>0) S->src_sounding[S->src][e][n]--; }
static int ArpEngine(int arp){ return arp < Arpeggiator::kEngines ? arp : S->seq.GetEngine(arp - Arpeggiator::kEngines); }
static void ArpNote(int arp, uint8_t note, uint8_t velocity){
    const int keep=S->src; S->src=arp;
    if(velocity>0){ if(S->arp_on[arp]>0) S->arp_overlap++; S->arp_on[arp]++; int e=ArpEngine(arp); S->g_arp_note_engine[arp][note]=(int8_t)(e+1); EngineNoteOn(e,note,velocity); }
    else { int e=S->g_arp_note_engine[arp][note]>0 ? S->g_arp_note_engine[arp][note]-1 : ArpEngine(arp); S->g_arp_note_engine[arp][note]=0; EngineNoteOff(e,note); if(S->arp_on[arp]>0) S->arp_on[arp]--; } S->src=keep; }
void LiveKeyOn(int arp, int e, uint8_t note, uint8_t velocity){
    S->g_arp_key[arp][note]=S->arp.IsOn(arp);
    if(S->g_arp_key[arp][note]) S->arp.KeyDown(arp,note,velocity); else { S->src=100; EngineNoteOn(e,note,velocity); S->src=-1; } }
void LiveKeyOff(int arp, int e, uint8_t note){
    if(S->g_arp_key[arp][note]){ S->g_arp_key[arp][note]=false; S->arp.KeyUp(arp,note);} else { S->src=100; EngineNoteOff(e,note); S->src=-1; } }
static void SeqNote(int track, int engine, uint8_t note, uint8_t velocity){
    const int arp=Arpeggiator::TrackArp(track);
    S->arp.SetPlayingPattern(track, S->seq.GetPattern(track));
    if(velocity>0){ S->g_seq_arp_note[track][note]=S->arp.IsOn(arp);
        if(S->g_seq_arp_note[track][note]) S->arp.KeyDown(arp,note,velocity,true); else { S->src=200+track; EngineNoteOn(engine,note,velocity); S->src=-1; } }
    else if(S->g_seq_arp_note[track][note]){ S->g_seq_arp_note[track][note]=false; S->arp.KeyUp(arp,note,true); }
    else { S->src=200+track; EngineNoteOff(engine,note); S->src=-1; } }
// main.cpp's ArpTick(), split in two here only so the log can stamp every
// note with this interrupt's tick: the clock (a function of the loop
// position published before the interrupt, so the same whether it's worked
// out before or after the sequencer), then the rest, after the sequencer,
// as in main.cpp.
static void ArpClock(uint32_t t_pub, uint32_t loop_ticks, bool running){
    Sim& s=*S;
    if(s.first){ s.last_count=s.timer_count; s.first=false; }
    const uint64_t now_count=s.timer_count; const float dt_us=(float)(uint32_t)(now_count-s.last_count)*1e6f/(float)kTickFreq; s.last_count=now_count;
    if(running && loop_ticks>0){ const uint32_t t=t_pub;
        if(!s.was_running){ s.tick=(s.tick/loop_ticks+1)*loop_ticks+t; s.prev=t; }
        uint32_t d = t>=s.prev ? t-s.prev : t+loop_ticks-s.prev; if(d>loop_ticks/2) s.tick=(s.tick/loop_ticks+1)*loop_ticks+t; else s.tick+=d; s.prev=t; }
    else { s.frac += s.tempo.GetBpm()*(float)Arpeggiator::kPpq/60e6f*dt_us; const uint32_t whole=(uint32_t)s.frac; s.tick+=whole; s.frac-=(float)whole; }
    s.was_running = running && loop_ticks>0;
}
static void ArpTick(bool running){
    Sim& s=*S;
    if(s.seq_was_running && !running) for(int t=0;t<8;t++) s.arp.Stop(Arpeggiator::TrackArp(t));
    s.seq_was_running=running;
    for(int t=0;t<8;t++){ s.arp.SetPlayingPattern(t,s.seq.GetPattern(t)); auto st=s.seq.GetState(t);
        bool playing = st==MidiSequencer::State::Playing || st==MidiSequencer::State::Recording;
        if(s.track_was_playing[t] && !playing) s.arp.Stop(Arpeggiator::TrackArp(t)); s.track_was_playing[t]=playing;
        const uint32_t pos=s.seq.GetPosTicks(t); if(running && playing && pos<s.track_pos[t]) s.arp.TrackLoopStart(t); s.track_pos[t]=pos; }
    s.arp.Tick(s.tick,true,(uint32_t)(s.us/1000.0));
}

static void AudioBlock(){
    Sim& s=*S;
    if(s.suspended){ if(s.seq_hold){ s.g_seq_tick=0; s.g_seq_running=false; } return; }
    for(int i=0;i<64;i++){ if(s.looper_enabled){ auto tk=s.tempo.Process(1.f); if(tk.record_start) s.g_record_starts++; } }
    s.g_seq_loop_ticks=(uint32_t)s.tempo.GetBars()*TempoClock::kBeatsPerBar*MidiSequencer::kPpq;
    s.g_seq_tick = s.looper_enabled ? s.tempo.GetLoopTick(MidiSequencer::kPpq) : 0;
    s.g_seq_running = s.looper_enabled;
}
// main.cpp's ControlTimerCallback(): the clock read once and given to both
// the sequencer and the arps -- or, with separate_reads, read for each (as
// it was), when the audio callback could publish the next tick in between.
static void ControlIsr(){
    Sim& s=*S;
    const uint32_t t=s.g_seq_tick, L=s.g_seq_loop_ticks; const bool run=s.g_seq_running;
    if(!s.separate_reads) ArpClock(t,L,run);
    s.seq.Tick(t,L,run,s.g_record_starts);
    if(s.preempt_now){ AudioBlock(); s.preempt_now=false; } // the audio callback, in between
    if(s.separate_reads){ ArpClock(s.g_seq_tick,s.g_seq_loop_ticks,s.g_seq_running); ArpTick(s.g_seq_running); }
    else ArpTick(run);
}
void Init(float bpm,int bars){
    delete S; // (one simulation at a time: the last one's freed)
    S=new Sim(); S->tempo.Init(48000.f); S->tempo.SetBpm(bpm); S->tempo.SetBars(bars);
    S->seq.Init(&SeqNote,S->store.data()); S->arp.Init(&ArpNote);
}
void Run(double seconds){
    Sim& s=*S; const double block_us=64/kRate*1e6; double next_block=s.us, next_isr=s.us; const double end=s.us+seconds*1e6;
    while(s.us<end){
        if(next_block<=next_isr){ s.us=next_block; s.sample=s.us*kRate/1e6; AudioBlock(); next_block+=block_us; }
        else { s.us=next_isr; s.sample=s.us*kRate/1e6; s.timer_count=(uint64_t)(s.us*kTickFreq/1e6)&0xffffffffull;
            s.preempt_now = s.preempt_us>0 && next_block<=next_isr+s.preempt_us;
            const bool took=s.preempt_now; ControlIsr(); if(took) next_block+=block_us; next_isr+=500.0; }
    }
}
void SetLooper(bool on){ if(!on) S->tempo.ResetPhase(); S->looper_enabled=on; }
