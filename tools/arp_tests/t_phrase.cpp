// Every factory phrase played through the real arp, step by step: the
// note and time of each step, rests and ties silent, ratchets, velocity
// order, held lengths, chance (fixed per start, about half over time) and
// nothing left sounding.
#include "harness.h"
#include <map>
#include <algorithm>
static int Run1(int p, int scale, bool lock, int root, int octave, std::vector<NoteLog>* out, double secs){
  Init(120,4); auto& s=S->arp.Get(1); s=Arpeggiator::Settings{}; s.on=true; s.mode=Arpeggiator::kPhrase; s.phrase=(uint8_t)p;
  s.scale=(uint8_t)scale; s.lock=lock; s.key=0; s.octave=(int8_t)octave; s.latch=true;
  Run(0.25); // land the key just after a step, so the arp starts at once
  const double step=6000; double t0=ceil(S->sample/step)*step; Run((t0-S->sample)/48000.0+0.0005);
  LiveKeyOn(1,1,(uint8_t)root,100); double start=S->sample; Run(secs);
  for(int k=0;k<128;k++) if(S->g_arp_key[1][k]) LiveKeyOff(1,1,k);
  s.on=false; Run(0.2);
  int stuck=0; for(int e=0;e<7;e++) for(int n=0;n<128;n++) stuck+=S->sounding[e][n];
  *out=S->log; for(auto& l:*out) l.sample-=start; return stuck;
}
int main(){
  int fails=0; const double step=6000; // 1/16 at 120 BPM
  for(int p=0;p<Arpeggiator::kNumPhrases;p++){
    const auto& ph=Arpeggiator::GetPhrase(p);
    for(int lock=0; lock<2; lock++){
      std::vector<NoteLog> log; const int root=lock?65:60; const int cycles=40;
      int stuck=Run1(p,0,lock,root,0,&log,(cycles*ph.len+0.5)*step/48000.0);
      Arpeggiator::Settings s; s.scale=0; s.lock=lock; s.key=0;
      int chance_steps=0, chance_played=0; std::map<int,int> vel_by_kind[4];
      for(int c=0;c<cycles;c++) for(int k=0;k<ph.len;k++){
        const auto& st=ph.steps[k]; double a=(c*ph.len+k)*step, b=a+step;
        std::vector<const NoteLog*> on; for(auto& l: log) if(l.vel>0 && l.sample>=a-step/4 && l.sample<b-step/4) on.push_back(&l);
        if(st.degree==Arpeggiator::kRest || st.degree==Arpeggiator::kTie){
          if(!on.empty()){ fails++; if(fails<6) printf("%s step %d: a note on a %s\n",ph.name,k,st.degree==Arpeggiator::kRest?"rest":"tie"); } continue; }
        if(st.chance){ chance_steps++; if(on.empty()) continue; chance_played++; }
        int want=Arpeggiator::PhraseNote(s,root,st.degree)+12*st.octave;
        if((int)on.size()!=st.ratchet){ fails++; if(fails<6) printf("%s step %d: %zu hits, want %d\n",ph.name,k,on.size(),st.ratchet); continue; }
        for(auto* l: on) if(l->note!=want){ fails++; if(fails<6) printf("%s step %d: note %d want %d\n",ph.name,k,l->note,want); }
        if(fabs(on[0]->sample-a)>48*4){ fails++; if(fails<6) printf("%s step %d: %.1f ms off\n",ph.name,k,(on[0]->sample-a)/48); }
        vel_by_kind[st.velocity][on[0]->vel]++;
        // held length: until its note off
        double off=-1; for(auto& l: log) if(l.vel==0 && l.note==want && l.sample>on[0]->sample){ off=l.sample; break; }
        int ties=0; for(int j=k+1;j<ph.len && ph.steps[j].degree==Arpeggiator::kTie;j++) ties++;
        double held=(off-on[0]->sample)/step;
        if(st.ratchet==1 && (st.length==2 || ties)){ if(held<ties+0.9){ fails++; if(fails<6) printf("%s step %d: long/tied note held %.2f steps\n",ph.name,k,held);} }
        else if(st.ratchet==1 && st.length==0 && held>0.45){ fails++; if(fails<6) printf("%s step %d: short note held %.2f\n",ph.name,k,held); }
      }
      // velocities: ghost < soft < normal < accent
      int prev=-1; for(int v=0;v<4;v++) if(!vel_by_kind[v].empty()){ int lo=vel_by_kind[v].begin()->first; if(lo<=prev){ fails++; printf("%s: velocity order wrong at kind %d\n",ph.name,v);} prev=vel_by_kind[v].rbegin()->first; }
      if(chance_steps>=20){ double r=(double)chance_played/chance_steps; if(r<0.25||r>0.75){ fails++; printf("%s: chance steps played %.0f%%\n",ph.name,r*100);} }
      if(stuck){ fails++; printf("%s: %d notes left sounding\n",ph.name,stuck); }
    }
    // chance: the same start plays the same
    std::vector<NoteLog> a,b; Run1(p,2,false,62,1,&a,ph.len*3*step/48000.0); Run1(p,2,false,62,1,&b,ph.len*3*step/48000.0);
    if(a.size()!=b.size()){ fails++; printf("%s: two runs from the same start differ\n",ph.name); }
  }
  printf("phrases: %d checks failed\n",fails);
  return fails?1:0;
}
