#include "harness.h"
#include <set>
static const char* N[12]={"C","C#","D","D#","E","F","F#","G","G#","A","A#","B"};
static void Notes(const char* label,double from,double to,int engine=-1,int max=12){
    printf("  %-30s",label); int n=0;
    for(auto& l: S->log) if(l.vel>0 && l.sample>=from && l.sample<to && (engine<0||l.engine==engine) && n<max){ printf("%s%d ",N[l.note%12],l.note/12-1); n++; }
    if(!n) printf("(silence)"); printf("\n");
}
int main(){
  // 1. pattern switch: A = C E G held all loop, B = F A C held all loop; arp on both
  { Init(120,1); MidiSequencer::TrackSave ts{}; ts.engine=1; ts.length_index=MidiSequencer::kDefaultLength; S->seq.LoadTrack(0,ts);
    MidiSequencer::Event a[]={{0,60,100,0,0},{0,64,100,0,0},{0,67,100,0,0},{380,60,0,0,0},{380,64,0,0,0},{380,67,0,0,0}};
    S->seq.AppendEvents(0,a,6);
    auto& A=S->arp.PatternSettings(0,0); A.on=true; A.latch=false; A.steps=4; A.range=1;
    Run(2.0);
    S->seq.RequestCopyPattern(0,1); S->arp.CopyPattern(0,0,1); Run(0.01);
    printf("1. pattern switch (loop = 1 bar = 2s)\n");
    double t0=S->sample; Run(4.0); Notes("after copy to B (same chords):",t0,S->sample,1,16);
  }
  // 2. latch + legato: hold C E G, press F A C 30ms before letting go of C E G
  { Init(120,4); auto& s=S->arp.Get(1); s.on=true; s.latch=true; s.lock=true; s.steps=6; s.range=1;
    printf("2. latch, legato chord change (engine arp, live)\n");
    LiveKeyOn(1,1,60,100); LiveKeyOn(1,1,64,100); LiveKeyOn(1,1,67,100); Run(1.0);
    LiveKeyOn(1,1,65,100); LiveKeyOn(1,1,69,100); LiveKeyOn(1,1,72,100); Run(0.03);
    LiveKeyOff(1,1,60); LiveKeyOff(1,1,64); LiveKeyOff(1,1,67); Run(0.3);
    LiveKeyOff(1,1,65); LiveKeyOff(1,1,69); LiveKeyOff(1,1,72);
    double t0=S->sample; Run(1.5); Notes("latched after letting go:",t0,S->sample);
    char nm[16]; S->arp.HeldChordName(1,nm,16); printf("  chord name: %s\n",nm);
  }
  // 3. rate change mid-play: gap between notes around the change
  { Init(120,4); auto& s=S->arp.Get(1); s.on=true; s.latch=true;
    printf("3. rate 1/16 -> 1/8T mid-play\n");
    LiveKeyOn(1,1,60,100); Run(1.03); s.rate=Arpeggiator::kRate8T; Run(1.0);
    double prev=-1, mingap=1e9; for(auto& l: S->log) if(l.vel>0){ if(prev>=0) mingap=std::min(mingap,l.sample-prev); prev=l.sample; }
    printf("  shortest gap between arp notes: %.1f ms (1/16 = 125, 1/8T = 167)\n",mingap/48.0);
  }
}
