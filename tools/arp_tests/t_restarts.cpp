#include "harness.h"
#include <random>
#include <algorithm>
static std::mt19937 rng(SEED);
static int R(int n){ return (int)(rng()%n); }
static void Load(int t,int engine,int base){
  MidiSequencer::TrackSave ts{}; ts.engine=engine; ts.length_index=MidiSequencer::kDefaultLength; S->seq.LoadTrack(t,ts);
  const int notes[12]={0,4,7,0,4,7,5,9,12,5,9,12}; const uint32_t ticks[12]={0,0,0,760,760,760,768,768,768,1530,1530,1530};
  MidiSequencer::Event a[12]; for(int i=0;i<12;i++){ a[i]=MidiSequencer::Event{}; a[i].tick=ticks[i]; a[i].note=(uint8_t)(base+notes[i]); a[i].velocity=(uint8_t)((i/3)%2==0?100:0); }
  S->seq.AppendEvents(t,a,12);
}
int main(){
  Init(120,4); Load(0,1,48); Load(1,2,60);
  for(int t=0;t<2;t++){ auto& s=S->arp.PatternSettings(t,0); s.on=true; s.latch=t==0; s.rate = t==0?Arpeggiator::kRate16:Arpeggiator::kRate8; }
  int bad_restarts=0, cycles=300;
  for(int c=0;c<cycles;c++){
    Run((300+R(5000))/1000.0);
    SetLooper(false); Run((100+R(3000))/1000.0);
    int what=R(6);
    if(what==0) S->tempo.SetBpm(80+R(80));
    if(what==1){ LiveKeyOn(Arpeggiator::TrackArp(0),1,50+R(20),100); Run(0.2); }
    if(what==2) S->seq.RequestPattern(R(2),0);
    SetLooper(true); double on_at=S->sample; Run(3.0);
    // each track's arp notes over 3s after restart must sit on its own grid from the restart
    const double spb=48000*60.0/S->tempo.GetBpm(); bool bad=false; double worst=0;
    for(auto& l: S->log) if(l.vel>0 && l.sample>=on_at && l.sample<on_at+3*48000){
      double step = l.engine==1 ? spb/4 : spb/2;
      double off=fmod(l.sample-on_at,step); if(off>step/2) off-=step; worst=std::max(worst,fabs(off)); if(fabs(off)>48*3) bad=true; }
    if(bad){ bad_restarts++; if(bad_restarts<=5) printf("cycle %d (did %d): worst %.1f ms off the grid\n",c,what,worst/48); }
    // release any live key
    for(int n=0;n<128;n++) if(S->g_arp_key[Arpeggiator::TrackArp(0)][n]) LiveKeyOff(Arpeggiator::TrackArp(0),1,n);
  }
  printf("%d restarts, %d off the grid\n",cycles,bad_restarts);
}
