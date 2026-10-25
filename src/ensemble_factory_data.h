#pragma once
#include <cstdint>

// Ensemble's own factory bank -- pure parameter numbers (7 drawbar
// levels, chorus depth/rate, envelope shape, filter), no audio at all,
// so genuinely zero licensing concern, same reasoning as Dexed's own
// factory patches (dexed_factory_data.h/.cpp). Hand-authored, not
// pulled from any external source.
struct EnsembleFactoryPreset
{
    const char* name;
    // 0=Saw8', 1=Sq8', 2=Saw4', 3=Sq4', 4=Saw2', 5=Sq2', 6=Saw1' --
    // same order as EnsembleSynth::SetDrawbarLevel()'s own idx.
    float       drawbar_levels[7];
    float       chorus_depth01;
    float       chorus_rate01;
    float       attack01;
    float       decay01;
    float       sustain01;
    float       release01;
    int32_t     filter_mode;
    float       filter_cutoff01;
    float       filter_res01;
    float       output_level01;
    float       pan01;
    float       reverb_send01; // into the shared reverb -- its Size is global (Global:Reverb)
    float       delay_send01;  // into the shared delay -- Time is global (Global:Delay)
    float       gain01;        // 0 = unity, 1 = +12 dB into the limiter
};

extern const EnsembleFactoryPreset kEnsembleFactoryPresets[];
extern const int                    kEnsembleNumFactoryPresets;
