#include "harness.h"
#include <random>
#include <algorithm>
static std::mt19937 rng(SEED);
static void Load(int t,int engine,int base){
  MidiSequencer::TrackSave ts{}; ts.engine=engine; ts.length_index=MidiSequencer::kDefaultLength; S->seq.LoadTrack(t,ts);
  const int notes[12]={0,4,7,0,4,7,5,9,12,5,9,12}; const uint32_t ticks[12]={0,0,0,760,760,760,768,768,768,1530,1530,1530};
  MidiSequencer::Event a[12]; for(int i=0;i<12;i++){ a[i]=MidiSequencer::Event{}; a[i].tick=ticks[i]; a[i].note=(uint8_t)(base+notes[i]); a[i].velocity=(uint8_t)((i/3)%2==0?100:0); }
  S->seq.AppendEvents(t,a,12);
}
// Ui::TriggerLoad(): clear the tracks, suspend audio (holding the sequencer), load tracks + arps, reset the tempo, resume
static void LoadPerf(bool hold){
  for(int t=0;t<8;t++) S->seq.RequestClear(t); Run(0.003);
  S->seq_hold=hold; S->suspended=true; Run(0.4);
  // the new performance: its own bars and tempo, as PerformanceStore::Load() sets them
  static int which=0; which^=1; S->tempo.SetBars(which?2:4); S->tempo.SetBpm(which?100:120);
  Load(0,1,48); Load(1,2,60);
  static Arpeggiator::SaveEntry e[Arpeggiator::kSaveEntries]; Arpeggiator src; src.Init(nullptr);
  for(int t=0;t<2;t++){ auto& s=src.PatternSettings(t,0); s.on=true; s.latch=t==0; s.steps=5; s.rate=t==0?Arpeggiator::kRate16:Arpeggiator::kRate8; }
  src.Save(e); S->arp.Load(e,Arpeggiator::kSaveEntries);
  Run(1.0); S->tempo.ResetPhase(); S->suspended=false; S->seq_hold=false;
}
int main(){
  for(int hold=0; hold<2; hold++){
    Init(120,4); int bad=0, late_start=0;
    LoadPerf(hold); Run(3.0);
    for(int c=0;c<200;c++){
      Run((rng()%16000)/1000.0+0.2);
      double before=S->sample; LoadPerf(hold); double on_at=S->sample; Run(3.0);
      bool b=false; bool first=true;
      for(auto& l: S->log) if(l.vel>0 && l.sample>=on_at && l.sample<on_at+3*48000){
        double spb=48000*60.0/S->tempo.GetBpm(); double step = l.engine==1 ? spb/4 : spb/2; double off=fmod(l.sample-on_at,step); if(off>step/2) off-=step;
        if(fabs(off)>48*3) b=true; if(first && l.engine==1){ first=false; if(l.sample-on_at>48*3) late_start++; } }
      if(first) late_start++; // track 1 never played
      if(b) bad++;
    }
    printf("%s: 200 loads mid-loop -> %d with arps off the grid, %d where track 1 didn't start on bar 1\n", hold?"with the fix":"without the fix",bad,late_start);
  }
}
