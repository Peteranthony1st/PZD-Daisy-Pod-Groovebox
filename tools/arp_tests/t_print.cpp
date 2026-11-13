// Print arp to notes: a track's arp, rendered and written into another
// pattern as notes (that pattern's arp off), must play exactly what the arp
// played -- the same notes on the same ticks for the same lengths.
#include "harness.h"
#include <random>
#include <set>
#include <tuple>
static std::mt19937 rng(SEED);
static int R(int n){ return (int)(rng()%n); }
typedef std::multiset<std::tuple<uint32_t,int,int>> Notes;
static Notes LoopNotes(uint32_t loop){
  // the last whole loop's notes on engine 1, by the arp clock's tick
  Notes out; uint64_t L=(S->tick/loop-1)*loop;
  for(size_t i=0;i<S->log.size();i++){ auto& l=S->log[i]; if(!l.vel || l.engine!=1 || l.tick<L-loop || l.tick>=L) continue;
    int len=-1; for(size_t j=i+1;j<S->log.size();j++) if(!S->log[j].vel && S->log[j].note==l.note && S->log[j].engine==1){ len=(int)(S->log[j].tick-l.tick); break; }
    if(len>=0) out.insert({(uint32_t)(l.tick%loop),l.note,len}); }
  return out;
}
int main(){
  int bad=0, cases=0, notes=0;
  for(int c=0;c<40;c++){
    Init(120,1+R(2)); const uint32_t loop=S->tempo.GetBars()*384; const double loop_s=loop*60.0/96/120.0;
    MidiSequencer::TrackSave ts{}; ts.engine=1; ts.length_index=MidiSequencer::kDefaultLength; ts.quant=(uint8_t)(R(2)?0:4); S->seq.LoadTrack(0,ts);
    std::vector<MidiSequencer::Event> ev; int chords=1+R(3);
    for(int k=0;k<chords;k++){ uint32_t at=(uint32_t)(k*loop/chords); uint32_t ln=loop/chords-12; int base=48+R(20);
      for(int j=0;j<1+R(3);j++){ MidiSequencer::Event on{}; on.tick=at; on.note=(uint8_t)(base+j*4); on.velocity=(uint8_t)(70+R(50)); auto off=on; off.velocity=0; off.tick=(at+ln)%loop; ev.push_back(on); ev.push_back(off);} }
    std::sort(ev.begin(),ev.end(),[](auto&a,auto&b){return a.tick<b.tick;});
    S->seq.AppendEvents(0,ev.data(),(int)ev.size());
    auto& s=S->arp.PatternSettings(0,0); s.on=true; s.latch=R(2); s.mode=(uint8_t)R(2); s.rate=(uint8_t)R(Arpeggiator::kNumRates);
    s.steps=(uint8_t)(1+R(16)); s.shape=(uint8_t)R(Arpeggiator::kNumShapes); s.chord=(uint8_t)R(Arpeggiator::kNumChords); s.lock=R(2);
    s.variation=(uint8_t)(R(2)?0:1+R(99)); s.swing01=R(2)?0.f:0.5f; s.gate01=0.3f+0.6f*(R(10)/10.f);
    if(s.mode==Arpeggiator::kPhrase){ for(int tries=0;tries<50;tries++){ s.phrase=(uint8_t)R(Arpeggiator::kNumPhrases); bool ch=false; auto& ph=Arpeggiator::GetPhrase(s.phrase); for(int k=0;k<ph.len;k++) if(ph.steps[k].chance && ph.steps[k].degree>=0) ch=true; if(!ch) break; } }
    Run(loop_s*5); Notes arp_notes=LoopNotes(loop);
    // print: as Ui::PrintArpToNotes() -- render, to events, into pattern B, B's arp off, switch to it
    static MidiSequencer::Event play[MidiSequencer::kMaxPlayEvents], scr[MidiSequencer::kMaxEvents], out[MidiSequencer::kMaxEvents]; static int16_t pa[MidiSequencer::kMaxEvents]; static int32_t sh[MidiSequencer::kMaxEvents];
    int n=S->seq.BuildPlayCopy(0,loop,play,MidiSequencer::kMaxPlayEvents,scr,pa,sh);
    static Arpeggiator::KeyEvent ke[MidiSequencer::kMaxPlayEvents]; for(int i=0;i<n;i++) ke[i]={play[i].tick,play[i].note,play[i].velocity};
    static Arpeggiator scratch; static Arpeggiator::RenderedNote rn[512]; int m=scratch.Render(s,ke,n,loop,rn,512);
    static MidiSequencer::NoteSpan sp[512]; for(int i=0;i<m;i++) sp[i]={rn[i].tick,rn[i].length,rn[i].note,rn[i].velocity};
    int ne=MidiSequencer::NotesToEvents(sp,m,loop,out,MidiSequencer::kMaxEvents);
    MidiSequencer::TrackSave pb{}; pb.engine=1; pb.length_index=MidiSequencer::kDefaultLength; S->seq.LoadPattern(0,1,pb); S->seq.AppendPatternEvents(0,1,out,ne);
    S->arp.CopyPattern(0,0,1); S->arp.PatternSettings(0,1).on=false;
    S->seq.RequestPattern(0,1);
    Run(loop_s*4); Notes printed=LoopNotes(loop);
    if(S->seq.GetPattern(0)!=1){ bad++; printf("case %d: didn't switch to B\n",c); continue; }
    auto near=[](const Notes& set,const std::tuple<uint32_t,int,int>& x){ for(int d=0;d<=0;d++) if(set.count({std::get<0>(x),std::get<1>(x),std::get<2>(x)+d})) return true; return false; };
    int miss=0,extra=0; for(auto& x: arp_notes) if(!near(printed,x)) miss++; for(auto& x: printed) if(!near(arp_notes,x)) extra++;
    cases++; notes+=(int)arp_notes.size();
    if(miss||extra){ bad++; if(bad<=4){ printf("case %d (mode %d latch %d rate %d): %d arp notes not printed, %d printed not played by the arp (%zu/%zu)\n",c,s.mode,s.latch,s.rate,miss,extra,arp_notes.size(),printed.size());
      int sh_=0; for(auto& x: arp_notes) if(!near(printed,x)&&sh_++<3) printf("   arp %u n%d len%d\n",std::get<0>(x),std::get<1>(x),std::get<2>(x));
      sh_=0; for(auto& x: printed) if(!near(arp_notes,x)&&sh_++<3) printf("   prn %u n%d len%d\n",std::get<0>(x),std::get<1>(x),std::get<2>(x)); } }
  }
  printf("print vs arp: %d cases, %d notes compared, %d cases differ\n",cases,notes,bad);
  return bad?1:0;
}
