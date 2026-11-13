#include "harness.h"
#include <map>
static const char* N[12]={"C","C#","D","D#","E","F","F#","G","G#","A","A#","B"};
int main(){
  Init(120,1);
  MidiSequencer::TrackSave ts{}; ts.engine=1; ts.length_index=MidiSequencer::kDefaultLength; S->seq.LoadTrack(0,ts);
  MidiSequencer::Event a[]={{0,60,100,0,0},{40,60,0,0,0}}; S->seq.AppendEvents(0,a,2);
  auto& s=S->arp.PatternSettings(0,0); s.on=true; s.latch=true; s.lock=true; s.steps=5; s.jump=4; s.shape=2; s.range=2;
  Run(10.0); SetLooper(false); Run(0.7); double r=S->sample; SetLooper(true); Run(6.0);
  // by loop (2s = 96000 samples, from each run's start), by step
  auto show=[&](const char* label,double start,int loops){
    for(int L=0;L<loops;L++){ printf("%s loop %d: ",label,L+1); std::map<int,int> m;
      for(auto& l: S->log) if(l.vel>0){ double d=l.sample-start-L*96000.0; if(d>=-200 && d<96000-200){ int step=(int)lround(d/6000.0); m[step]=l.note; } }
      for(int st=0;st<16;st++) printf("%s ", m.count(st)? N[m[st]%12] : "."); printf("\n"); } };
  show("from power-on",0,5); show("after restart",r,3);
}
