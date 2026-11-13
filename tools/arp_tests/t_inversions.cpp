// Follow mode with chords played the many ways a hand plays them: every
// inversion and spread voicing, the keys in every order, landing up to
// 30 ms apart, before or after a step. After the chord settles every note
// the arp plays must be one of the chord's -- and the root position pool
// it walks must be the chord's whatever the voicing.
#include "harness.h"
#include <algorithm>
#include <vector>
#include <set>
static const char* N[12]={"C","C#","D","D#","E","F","F#","G","G#","A","A#","B"};
int main(){
  struct Ch { const char* name; std::vector<int> pcs; } chords[]={
    {"C",{0,4,7}},{"Cm",{0,3,7}},{"C7",{0,4,7,10}},{"Cmaj7",{0,4,7,11}},{"Cm7",{0,3,7,10}},{"Cdim",{0,3,6}},{"Csus4",{0,5,7}},{"Cadd9",{0,2,4,7}}};
  int bad=0, trials=0; std::set<std::string> fails;
  for(auto& ch: chords){
    // voicings: each inversion (lowest note rotated), close and spread an octave
    std::vector<std::vector<int>> voicings;
    for(size_t inv=0; inv<ch.pcs.size(); inv++) for(int spread=0; spread<2; spread++){
      std::vector<int> v; int prev=-1;
      for(size_t k=0;k<ch.pcs.size();k++){ int pc=ch.pcs[(inv+k)%ch.pcs.size()]; int n=48+pc; while(n<=prev) n+=12; if(spread && k==1) n+=12; v.push_back(n); prev=n; }
      voicings.push_back(v); }
    for(auto& v: voicings){
      std::vector<int> order=v; std::sort(order.begin(),order.end());
      do {
        for(int gap_ms: {0,8,20,30}) for(int offset_ms: {-20,-5,3,30}) {
          Init(120,4); auto& s=S->arp.Get(1); s=Arpeggiator::Settings{}; s.on=true; s.lock=false; s.latch=true; s.range=1; s.steps=8;
          Run(1.0); const double step=6000; double next=ceil(S->sample/step)*step+step; Run((next-S->sample)/48000.0+offset_ms/1000.0);
          size_t from=S->log.size();
          for(size_t k=0;k<order.size();k++){ LiveKeyOn(1,1,(uint8_t)order[k],100); if(k+1<order.size()) Run(gap_ms/1000.0); }
          double settled=S->sample+48*45; Run(1.0);
          // after it settles: every note one of the chord's pitch classes
          std::set<int> heard; bool wrong=false;
          for(size_t i=from;i<S->log.size();i++){ auto& l=S->log[i]; if(!l.vel || l.sample<settled) continue; heard.insert(l.note%12);
            if(std::find(ch.pcs.begin(),ch.pcs.end(),l.note%12)==ch.pcs.end()) wrong=true; }
          char nm[16]; S->arp.HeldChordName(1,nm,16);
          bool named = std::string(nm).rfind(ch.name,0)==0; // "C", "C/E" ...
          trials++;
          if(wrong || !named){ bad++; char key[96]; std::string o; for(int x: order) o+=std::string(N[x%12])+" ";
            snprintf(key,sizeof(key),"%s: keys %sgap %d offset %d -> %s%s",ch.name,o.c_str(),gap_ms,offset_ms,nm,wrong?" (wrong notes)":"");
            fails.insert(key); }
        }
      } while(std::next_permutation(order.begin(),order.end()));
    }
  }
  int shown=0; for(auto& f: fails) if(shown++<12) printf("  %s\n",f.c_str());
  printf("inversions: %d trials, %d wrong\n",trials,bad);
  return bad?1:0;
}
