#include "harness.h"
#include <random>
#include <set>
static std::mt19937 rng(SEED);
static int R(int n){ return (int)(rng()%n); }
static void LoadTrack(int t,int engine){
  MidiSequencer::TrackSave ts{}; ts.engine=engine; ts.length_index=MidiSequencer::kDefaultLength; S->seq.LoadTrack(t,ts);
  const uint8_t b=(uint8_t)(t*5);
  MidiSequencer::Event a[12]; const int notes[12]={48,52,55,48,52,55,53,57,60,53,57,60}; const uint32_t ticks[12]={0,0,0,700,700,700,768,768,768,1500,1500,1500};
  for(int i=0;i<12;i++){ a[i]=MidiSequencer::Event{}; a[i].tick=ticks[i]; a[i].note=(uint8_t)(notes[i]+b); a[i].velocity=(uint8_t)((i/3)%2==0?100:0); }
  S->seq.AppendEvents(t,a,12);
}
static void RandomSetting(Arpeggiator::Settings& s){
  switch(R(16)){
    case 0: s.on=R(2); break; case 1: s.mode=R(Arpeggiator::kNumModes); break; case 2: s.rate=R(Arpeggiator::kNumRates); break;
    case 3: s.latch=R(2); break; case 4: s.steps=1+R(Arpeggiator::kMaxGenSteps); break; case 5: s.jump=1+R(Arpeggiator::kMaxJump); break;
    case 6: s.shape=R(Arpeggiator::kNumShapes); break; case 7: s.repeat=1+R(Arpeggiator::kMaxRepeat); break;
    case 8: s.phrase=R(Arpeggiator::kNumPhrases); break; case 9: s.variation=R(Arpeggiator::kNumVariations); break;
    case 10: s.key=R(12); break; case 11: s.scale=R(Arpeggiator::kNumScales); break; case 12: s.lock=R(2); break;
    case 13: s.range=1+R(Arpeggiator::kMaxRange); break; case 14: s.chord=R(Arpeggiator::kNumChords); break;
    case 15: s.gate01=0.05f+0.95f*(R(100)/100.f); s.swing01=R(100)/100.f; s.octave=(int8_t)(R(5)-2); break; }
}
int main(){
  Init(120,2); for(int t=0;t<4;t++) LoadTrack(t,R(7));
  for(int t=0;t<4;t++){ S->seq.RequestCopyPattern(t,1); S->arp.CopyPattern(t,0,1); }
  for(int e=0;e<Arpeggiator::kInstances;e++){ auto& s = e<7 ? S->arp.Get(e) : S->arp.PatternSettings(e-7,0); s.on=true; }
  bool down[Arpeggiator::kInstances][128]={}; int keyeng[Arpeggiator::kInstances][128]={};
  static Arpeggiator::SaveEntry buf[Arpeggiator::kSaveEntries];
  double sim=0; int actions=0;
  while(sim<600){
    double dt=(20+R(300))/1000.0; Run(dt); sim+=dt; actions++;
    int a=R(100);
    int inst=R(Arpeggiator::kInstances); int e = inst<7 ? inst : S->seq.GetEngine(inst-7); int n=36+R(48);
    if(a<35){ if(!down[inst][n]){ LiveKeyOn(inst,e,n,1+R(127)); down[inst][n]=true; keyeng[inst][n]=e; } }
    else if(a<65){ for(int k=0;k<128;k++) if(down[inst][k] && R(2)){ LiveKeyOff(inst,keyeng[inst][k],k); down[inst][k]=false; } }
    else if(a<85){ auto& s = inst<7 ? S->arp.Get(inst) : S->arp.PatternSettings(inst-7,R(8)); RandomSetting(s); }
    else if(a<89){ S->seq.RequestPattern(R(4),R(2)); }
    else if(a<91){ SetLooper(!S->looper_enabled); }
    else if(a<93){ S->arp.Save(buf); S->arp.Load(buf,Arpeggiator::kSaveEntries); }
    else if(a<94){ S->arp.Reset(); }
    else if(a<96){ S->seq.SetEngine(R(4),R(7)); }
    else if(a<98){ S->arp.CopyToAllPatterns(R(8),R(8)); }
    else { char nm[16]; S->arp.HeldChordName(inst,nm,16); int pool[Arpeggiator::kMaxPool]; S->arp.CurrentPool(inst,pool); }
  }
  // wind down: release every key, everything off, stop the looper
  for(int i=0;i<Arpeggiator::kInstances;i++) for(int k=0;k<128;k++) if(down[i][k]){ LiveKeyOff(i,keyeng[i][k],k); }
  for(int e=0;e<7;e++) S->arp.Get(e).on=false;
  for(int t=0;t<8;t++) for(int p=0;p<8;p++) S->arp.PatternSettings(t,p).on=false;
  SetLooper(false); Run(1.0);
  int stuck=0, bad=0, ons=0; for(int e=0;e<7;e++) for(int k=0;k<128;k++) stuck+=S->sounding[e][k];
  for(auto& l: S->log){ if(l.note<0||l.note>127||l.vel<0||l.vel>127||l.engine<0||l.engine>6) bad++; if(l.vel>0) ons++; }
  // double triggers: same engine+note on twice with no off between
  int dbl=0; { int st[7][128]={}; for(auto& l: S->log){ if(l.vel>0){ if(st[l.engine][l.note]>0) dbl++; st[l.engine][l.note]++; } else if(st[l.engine][l.note]>0) st[l.engine][l.note]--; } }
  printf("stuck by source:"); for(int src=0;src<300;src++){ int c=0; for(int e=0;e<7;e++) for(int k=0;k<128;k++) c+=S->src_sounding[src][e][k]; if(c) printf(" [%s%d: %d]", src<100?"arp":src<200?"live":"seq", src<100?src:src<200?0:src-200, c); } printf("\n");
  printf("an arp with two notes on at once: %d\n", S->arp_overlap);
  printf("%d actions over %.0fs simulated, %d notes played\n",actions,sim,ons);
  printf("notes left sounding: %d   invalid notes: %d   on-on without off (same engine+note): %d\n",stuck,bad,dbl);
}
