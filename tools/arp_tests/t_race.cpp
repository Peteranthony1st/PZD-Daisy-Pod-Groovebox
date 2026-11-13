// PERF16's two arps (M1, M2 changing chord on the same ticks each bar),
// with the audio callback able to land inside the control interrupt: the
// first arp note of each bar must be the new chord's. Read once per
// interrupt (now) it always is; read separately for the sequencer and the
// arp clock (as it was), the arp could play a bar's first step before the
// sequencer had sent the bar's chord -- the old chord's note on the bar line.
#include "harness.h"
#include <vector>
static MidiSequencer::Event E(uint32_t t,int n,int v){ MidiSequencer::Event e{}; e.tick=t; e.note=(uint8_t)n; e.velocity=(uint8_t)v; return e; }
static void Track(int t,int engine,int tp,std::vector<MidiSequencer::Event> ev){
  MidiSequencer::TrackSave ts{}; ts.engine=(uint8_t)engine; ts.length_index=2; ts.quant=4; S->seq.LoadTrack(t,ts);
  S->seq.AppendEvents(t,ev.data(),(int)ev.size()); S->seq.SetTranspose(t,0,tp); }
static int Run1(bool separate, double preempt_us, int loops, int* bars){
  Init(100,4); S->separate_reads=separate; S->preempt_us=preempt_us;
  Track(0,1,-12,{E(0,72,100),E(7,72,0),E(384,79,100),E(408,79,0),E(768,81,100),E(792,81,0),E(1152,77,100),E(1176,77,0)});
  Track(1,2,0,{E(0,72,100),E(120,72,0),E(372,79,100),E(410,79,0),E(768,81,100),E(792,81,0),E(1152,77,100),E(1176,77,0)});
  auto& a=S->arp.PatternSettings(0,0); a.on=true; a.rate=3; a.latch=true; a.steps=4; a.jump=6; a.shape=2; a.repeat=2; a.lock=true; a.range=1; a.gate01=0.6f;
  auto& b=S->arp.PatternSettings(1,0); b.on=true; b.rate=3; b.latch=true; b.steps=8; b.lock=true; b.range=1; b.gate01=0.6f;
  Run(1536*60.0/96/100.0*loops);
  // M2 (engine 2): its note on each bar line must be that bar's chord's root
  const int want[4]={72,79,81,77}; int wrong=0; *bars=0;
  for(auto& l: S->log){ if(!l.vel || l.engine!=2 || l.tick%384) continue;
    int bar=(int)((l.tick%1536)/384); (*bars)++; if(l.note%12!=want[bar]%12) wrong++; }
  return wrong;
}
int main(){
  int bars;
  int old_wrong=Run1(true,60,200,&bars);
  printf("as it was (read twice, audio can land between): %d of %d bar lines played the old chord\n",old_wrong,bars);
  int new_wrong=Run1(false,60,200,&bars);
  printf("now (read once per interrupt):                  %d of %d bar lines played the old chord\n",new_wrong,bars);
  return new_wrong?1:0;
}
