#include "harness.h"
#include <algorithm>
static void LoadChords(int t, bool latch_hint){
    MidiSequencer::TrackSave ts{}; ts.engine=1; ts.length_index=MidiSequencer::kDefaultLength; S->seq.LoadTrack(t,ts);
    MidiSequencer::Event ev[]={{0,60,100,0,0},{0,64,100,0,0},{0,67,100,0,0},{760,60,0,0,0},{760,64,0,0,0},{760,67,0,0,0},
                               {768,65,100,0,0},{768,69,100,0,0},{768,72,100,0,0},{1530,65,0,0,0},{1530,69,0,0,0},{1530,72,0,0,0}};
    S->seq.AppendEvents(t,ev,12);
}
static void Check(const char* name, double from, double to, double restart, double step){
    int n=0; double worst=0; int offgrid=0;
    for(auto& l: S->log) if(l.vel>0 && l.sample>=from && l.sample<to){ n++;
        double off=fmod(l.sample-restart, step); if(off>step/2) off-=step; double a=fabs(off); worst=std::max(worst,a); if(a>200) offgrid++; }
    printf("  %-28s notes=%3d  worst offset=%6.0f samples (%.1f ms)  off-grid(>4ms)=%d\n",name,n,worst,worst/48.0,offgrid);
}
int main(int argc,char**argv){
    for(int latch=1; latch>=0; latch--){
    Init(120,4); LoadChords(0,latch);
    auto& s=S->arp.PatternSettings(0,0); s.on=true; s.latch=latch;
    const double step=6000; // 1/16 at 120bpm
    printf("latch %s\n", latch?"on":"off");
    Run(16.0); Check("first two loops",0,16*48000.0,0,step);
    SetLooper(false); double off_at=S->sample; Run(1.3);
    int during=0; for(auto& l: S->log) if(l.vel>0 && l.sample>off_at+48*5 && l.sample<S->sample) during++;
    printf("  notes while looper off: %d\n",during);
    SetLooper(true); double on_at=S->sample; Run(16.0); Check("after restart",on_at,S->sample,on_at,step);
    // first arp note after restart
    for(auto& l: S->log) if(l.vel>0 && l.sample>=on_at){ printf("  first note after restart: +%.1f ms (note %d)\n",(l.sample-on_at)/48.0,l.note); break; }
    SetLooper(false); Run(0.5);
    int stuck=0; for(int e=0;e<7;e++) for(int n=0;n<128;n++) stuck+=S->sounding[e][n]; printf("  notes left sounding after stop: %d\n",stuck);
    }
}
