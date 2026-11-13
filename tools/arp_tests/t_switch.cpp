#include "harness.h"
int main(){
  for(int latch=0; latch<2; latch++){
    Init(120,1); MidiSequencer::TrackSave ts{}; ts.engine=1; ts.length_index=MidiSequencer::kDefaultLength; S->seq.LoadTrack(0,ts);
    MidiSequencer::Event a[]={{0,60,100,0,0},{0,64,100,0,0},{0,67,100,0,0},{380,60,0,0,0},{380,64,0,0,0},{380,67,0,0,0}};
    S->seq.AppendEvents(0,a,6);
    auto& A=S->arp.PatternSettings(0,0); A.on=true; A.latch=latch; A.steps=4; A.range=1;
    Run(1.0);
    S->seq.RequestCopyPattern(0,1); S->arp.CopyPattern(0,0,1); // B = copy of A, notes and arp
    Run(4.0); // B now playing
    printf("latch %s, playing %c;", latch?"on ":"off", 'A'+S->seq.GetPattern(0));
    S->seq.RequestPattern(0,0); Run(0.001);
    double bar=96000, next=ceil(S->sample/bar)*bar; Run((next-S->sample)/48000.0+6.0);
    printf(" switch to A at %.0fs: notes per bar:",next/48000);
    for(int b=-1;b<3;b++){ int n=0; for(auto& l: S->log) if(l.vel>0 && l.sample>=next+b*bar && l.sample<next+(b+1)*bar) n++; printf(" %d",n); }
    printf("  (bar before, then 3 bars; 16 each expected)\n");
  }
}
