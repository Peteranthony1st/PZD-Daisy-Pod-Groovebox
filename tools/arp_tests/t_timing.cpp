#include "harness.h"
#include <algorithm>
static const char* N[12]={"C","C#","D","D#","E","F","F#","G","G#","A","A#","B"};
static void Live(int t,int e,int note,int vel){ // main.cpp: a key while track t is selected
  if(vel){ LiveKeyOn(Arpeggiator::TrackArp(t),e,note,vel); S->seq.OnLiveNote(t,note,vel); }
  else { LiveKeyOff(Arpeggiator::TrackArp(t),e,note); S->seq.OnLiveNote(t,note,0); } }
int main(){
  // T1: free-running, looper off, 60 s
  { Init(120,4); SetLooper(false); auto& s=S->arp.Get(1); s.on=true; s.latch=true;
    LiveKeyOn(1,1,60,100); Run(60.0);
    std::vector<double> on; for(auto& l: S->log) if(l.vel>0) on.push_back(l.sample);
    double worst=0; for(size_t i=1;i<on.size();i++) worst=std::max(worst,fabs(on[i]-on[i-1]-6000));
    double drift=on.back()-(on.front()+(on.size()-1)*6000.0);
    printf("T1 looper off, 60s: %zu notes, worst step error %.2f ms, total drift %.2f ms (timer wraps every ~17.9s)\n",on.size(),worst/48,drift/48); }
  // T2: swing
  { Init(120,4); auto& s=S->arp.Get(1); s.on=true; s.latch=true; s.swing01=0.5f; // shows as 62%
    LiveKeyOn(1,1,60,100); Run(4.0);
    double even=0,odd=0; int ne=0,no=0; for(auto& l: S->log) if(l.vel>0 && l.sample>48000){
      long slot=(long)floor((l.sample+300)/6000.0); double off=l.sample-slot*6000.0; if(slot%2){odd+=off;no++;} else {even+=off;ne++;} }
    printf("T2 swing 62%%: on-beat steps late by %.1f ms, off-beat steps late by %.1f ms (expected 0 and %.1f)\n",even/ne/48,odd/no/48,1500/48.0); }
  // T3: record through the track's arp, then play back
  { Init(120,2); MidiSequencer::TrackSave ts{}; ts.engine=1; ts.length_index=MidiSequencer::kDefaultLength; S->seq.LoadTrack(0,ts);
    auto& a=S->arp.PatternSettings(0,0); a.on=true; a.latch=false; a.lock=true; a.steps=4; a.range=1;
    Run(0.5); S->tempo.RequestRecordStart(); S->seq.RequestRecord(0);
    uint32_t rs=S->g_record_starts; while(S->g_record_starts==rs) Run(0.0005);
    double rec0=S->sample; printf("T3 recording starts at %.3fs\n",rec0/48000);
    const double bar=96000;
    // play C E G on beat 1 (a strum, 10ms apart), hold to just before bar 2; F A C on bar 2 played 15ms late
    Live(0,1,60,100); Run(0.01); Live(0,1,64,100); Run(0.01); Live(0,1,67,100);
    Run((rec0+bar-48*30-S->sample)/48000); Live(0,1,60,0); Live(0,1,64,0); Live(0,1,67,0);
    Run((rec0+bar+48*15-S->sample)/48000); Live(0,1,65,100); Live(0,1,69,100); Live(0,1,72,100);
    Run((rec0+2*bar-48*30-S->sample)/48000); Live(0,1,65,0); Live(0,1,69,0); Live(0,1,72,0);
    Run(0.2);
    printf("   state after the pass: %s, %d events\n", S->seq.GetState(0)==MidiSequencer::State::Playing?"Playing":"other", S->seq.GetEventCount(0));
    const MidiSequencer::Event* ev=S->seq.GetEvents(0);
    printf("   recorded: "); for(int i=0;i<S->seq.GetEventCount(0);i++) if(ev[i].velocity) printf("%s@%u ",N[ev[i].note%12],ev[i].tick); printf("\n");
    printf("   what played while recording, by step:\n   ");
    for(auto& l: S->log) if(l.vel>0 && l.sample>=rec0 && l.sample<rec0+2*bar) printf("%s%+.0f ",N[l.note%12],fmod(l.sample-rec0+3000,6000)/48-62.5);
    printf("\n   (ms off the 1/16 grid)\n");
    double pb0=rec0+2*bar; Run(4.2);
    printf("   playback, next loop:\n   ");
    for(auto& l: S->log) if(l.vel>0 && l.sample>=pb0 && l.sample<pb0+2*bar) printf("%s%+.0f ",N[l.note%12],fmod(l.sample-pb0+3000,6000)/48-62.5);
    printf("\n");
  }
}
