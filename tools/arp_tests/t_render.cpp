// The offline render (what the grid's ghost notes and Print-to-notes use)
// against the live arp: random patterns on a track, played through the
// real sequencer and arp, and rendered offline from BuildPlayCopy() --
// the same notes on the same ticks for the same lengths.
#include "harness.h"
#include <random>
#include <set>
#include <tuple>
static std::mt19937 rng(SEED);
static int R(int n){ return (int)(rng()%n); }
int main(){
  int bad=0, cases=0, compared=0;
  for(int c=0;c<60;c++){
    Init(120, 1+R(2)); // 1 or 2 bars
    MidiSequencer::TrackSave ts{}; ts.engine=1; ts.length_index=MidiSequencer::kDefaultLength; ts.quant=(uint8_t)(R(3)?0:4); ts.swing=(uint8_t)(R(2)?0:50);
    S->seq.LoadTrack(0,ts);
    // a random chord progression: 1-4 chords of 1-3 keys, held for a while
    std::vector<MidiSequencer::Event> ev; const uint32_t loop=S->tempo.GetBars()*384;
    int chords=1+R(4); for(int k=0;k<chords;k++){ uint32_t at=(uint32_t)(k*loop/chords + R(2)*R(7)); uint32_t ln=loop/chords-10-R(30);
      int nk=1+R(3); int base=48+R(24);
      for(int j=0;j<nk;j++){ MidiSequencer::Event on{}; on.tick=at+j*R(3); on.note=(uint8_t)(base+j*(3+R(3))); on.velocity=(uint8_t)(60+R(60));
        MidiSequencer::Event off=on; off.velocity=0; off.tick=(on.tick+ln)%loop; ev.push_back(on); ev.push_back(off);} }
    std::sort(ev.begin(),ev.end(),[](auto&a,auto&b){return a.tick<b.tick;});
    S->seq.AppendEvents(0,ev.data(),(int)ev.size());
    if(R(2)) S->seq.SetTranspose(0,0,R(5)-2);
    auto& s=S->arp.PatternSettings(0,0); s.on=true; s.latch=R(2); s.mode=(uint8_t)R(2); s.rate=(uint8_t)R(Arpeggiator::kNumRates);
    s.steps=(uint8_t)(1+R(16)); s.jump=(uint8_t)(1+R(3)); s.shape=(uint8_t)R(Arpeggiator::kNumShapes); s.repeat=(uint8_t)(1+R(3));
    s.chord=(uint8_t)R(Arpeggiator::kNumChords); s.lock=R(2); s.scale=(uint8_t)R(Arpeggiator::kNumScales); s.range=(uint8_t)(1+R(3));
    s.variation=(uint8_t)(R(3)?0:1+R(99)); s.swing01=R(2)?0.f:0.5f; s.gate01=0.2f+0.7f*(R(10)/10.f);
    // Phrase mode: only phrases without chance steps (chance throws depend on how many times round)
    if(s.mode==Arpeggiator::kPhrase){ for(int tries=0;tries<50;tries++){ s.phrase=(uint8_t)R(Arpeggiator::kNumPhrases); bool ch=false; auto& ph=Arpeggiator::GetPhrase(s.phrase); for(int k=0;k<ph.len;k++) if(ph.steps[k].chance && ph.steps[k].degree>=0) ch=true; if(!ch) break; } s.octave=(int8_t)(R(3)-1); }
    // live: five loops; the fourth's notes (by the arp's tick, which runs
    // in step with the loop), each with its length from its note off.
    const double loop_s=loop*60.0/96/120.0;
    Run(loop_s*5);
    uint64_t L=0; for(auto& l: S->log) if(l.tick/loop>=3){ L=(l.tick/loop)*loop; break; }
    L=(S->tick/loop-1)*loop; // the last whole loop before now
    std::multiset<std::tuple<uint32_t,int,int>> live;
    for(size_t i=0;i<S->log.size();i++){ auto& l=S->log[i]; if(!l.vel || l.engine!=1 || l.tick<L-loop || l.tick>=L) continue;
      int len=-1; for(size_t j=i+1;j<S->log.size();j++) if(!S->log[j].vel && S->log[j].note==l.note && S->log[j].engine==1){ len=(int)(S->log[j].tick-l.tick); break; }
      if(len<0) continue;
      live.insert({(uint32_t)(l.tick%loop),l.note,len}); }
    // offline: BuildPlayCopy -> Render
    static MidiSequencer::Event play[MidiSequencer::kMaxPlayEvents], scratch[MidiSequencer::kMaxEvents]; static int16_t pa[MidiSequencer::kMaxEvents]; static int32_t sh[MidiSequencer::kMaxEvents];
    int n=S->seq.BuildPlayCopy(0,loop,play,MidiSequencer::kMaxPlayEvents,scratch,pa,sh);
    static Arpeggiator::KeyEvent ke[MidiSequencer::kMaxPlayEvents]; for(int i=0;i<n;i++) ke[i]={play[i].tick,play[i].note,play[i].velocity};
    static Arpeggiator scratch_arp; static Arpeggiator::RenderedNote out[1024];
    int m=scratch_arp.Render(s,ke,n,loop,out,1024);
    std::multiset<std::tuple<uint32_t,int,int>> off;
    for(int i=0;i<m;i++) off.insert({out[i].tick,out[i].note,(int)out[i].length});
    // Lengths match to a tick: the harness stamps a note off from the
    // track's key up with the arp clock's tick, not yet moved on for that
    // interrupt, so it can read one early.
    auto near=[](const std::multiset<std::tuple<uint32_t,int,int>>& set, const std::tuple<uint32_t,int,int>& x){
      for(int d=0; d<=0; d++) if(set.count({std::get<0>(x),std::get<1>(x),std::get<2>(x)+d})) return true; return false; };
    int miss=0; for(auto& x: live){ if(!near(off,x)) miss++; }
    int extra=0; for(auto& x: off){ if(!near(live,x)) extra++; }
    cases++; compared+=(int)live.size();
    if(miss||extra){ bad++; if(bad<=4){ printf("case %d (mode %d latch %d rate %d swing %.1f quant %d): %d live notes not rendered, %d rendered not live (of %zu/%zu)\n",c,s.mode,s.latch,s.rate,s.swing01,ts.quant,miss,extra,live.size(),off.size());
        int shown=0; for(auto& x: live) if(!near(off,x) && shown++<3) printf("   live %u n%d len%d\n",std::get<0>(x),std::get<1>(x),std::get<2>(x));
        shown=0; for(auto& x: off) if(!near(live,x) && shown++<3) printf("   rend %u n%d len%d\n",std::get<0>(x),std::get<1>(x),std::get<2>(x)); } }
  }
  printf("render vs live: %d cases, %d notes compared, %d cases differ\n",cases,compared,bad);
  return bad?1:0;
}
