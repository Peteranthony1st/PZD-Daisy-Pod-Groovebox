// Chord changes while the arp runs, as on other arps: a new chord is taken
// at the next step -- every note starts on a step, plays its whole gate, and
// a step is never struck twice, however the keys land.
#include "harness.h"
#include <random>
#include <map>
#ifndef SEED
#define SEED 1
#endif
static std::mt19937 rng(SEED);
static int R(int n){ return (int)(rng()%n); }
int main(){
  int bad_grid=0, bad_cut=0, bad_double=0, notes=0;
  for(int c=0;c<30;c++){
    Init(120,4); auto& s=S->arp.Get(1); s=Arpeggiator::Settings{}; s.on=true; s.latch=R(2); s.lock=R(2);
    s.rate=Arpeggiator::kRate16; s.gate01=0.6f; s.shape=(uint8_t)R(Arpeggiator::kNumShapes);
    Run(0.5);
    std::vector<int> held;
    for(int k=0;k<40;k++){ // a chord change every 80..400 ms, its keys up to 25 ms apart
      for(int n: held) LiveKeyOff(1,1,(uint8_t)n); held.clear();
      int root=48+R(12), nk=1+R(3);
      for(int j=0;j<nk;j++){ int n=root+j*(3+R(2)); LiveKeyOn(1,1,(uint8_t)n,100); held.push_back(n); Run(R(26)/1000.0); }
      Run((80+R(320))/1000.0);
    }
    for(int n: held) LiveKeyOff(1,1,(uint8_t)n); Run(0.5);
    const double step_ticks=24;
    std::map<uint64_t,int> per_step; uint64_t prev_on=0; bool first=true;
    for(size_t i=0;i<S->log.size();i++){ auto& l=S->log[i]; if(!l.vel) continue; notes++;
      // on the arp's step grid (by its clock, exact) -- but for a first note
      // after the arp stopped, which starts at once if the key came just after a step
      const bool restart = first || l.tick-prev_on > (uint64_t)(step_ticks*1.5);
      first=false; prev_on=l.tick;
      if(l.tick%24 && !restart){ bad_grid++; if(bad_grid<=3) printf("  off grid: tick %llu\n",(unsigned long long)l.tick); continue; }
      if(++per_step[l.tick/24]>1){ bad_double++; if(bad_double<=3) printf("  step struck twice: tick %llu\n",(unsigned long long)l.tick); }
      // its note off: its whole gate, unless the keys were all let go (latch off)
      for(size_t j=i+1;j<S->log.size();j++) if(!S->log[j].vel && S->log[j].note==l.note){
        const uint64_t held_for=S->log[j].tick-l.tick;
        if(held_for+1 < (uint64_t)(0.6*step_ticks) && s.latch){ bad_cut++; if(bad_cut<=5) printf("  cut short: on tick %llu held %llu ticks (latch %d)\n",(unsigned long long)l.tick,(unsigned long long)held_for,s.latch); }
        break; }
    }
  }
  printf("chord changes: %d notes; %d off the step grid, %d steps struck twice, %d notes cut short by a change\n",notes,bad_grid,bad_double,bad_cut);
  return (bad_grid||bad_double||bad_cut)?1:0;
}
