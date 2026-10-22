#include "ensemble_factory_data.h"

// Hand-authored, untuned starting points -- real parameter numbers
// chosen to sound like distinct, characteristic examples of this
// instrument's own range (warm/fundamental-heavy through bright/
// buzzy, slow pads through faster plucks), not yet verified against
// real hardware playing (see the plan's own Verification step).
// Each preset's own 7 drawbar levels (0=Saw8', 1=Sq8', 2=Saw4', 3=Sq4',
// 4=Saw2', 5=Sq2', 6=Saw1') were carried over from this bank's earlier
// single-macro design (interpolating its own old Warm/Bright
// registration pair at each preset's own macro position), now that
// EnsembleSynth exposes real per-drawbar control instead -- so the
// same intended character is preserved, just expressed directly.
// {name, {drawbar_levels[7]}, chorus_depth01, chorus_rate01,
//  attack01, decay01, sustain01, release01,
//  filter_mode (0=Off, 1=LowPass), filter_cutoff01, filter_res01,
//  output_level01, pan01}
// output_level01 bumped up on every preset (0.8/0.85 -> 0.95/1.0, a flat
// +0.15 each, capped at the real max) per direct request -- a clean
// loudness increase, not a distortion one: it's applied AFTER the
// per-voice tanhf() soft limiter (see Process()'s own eff_output_level_
// multiply), so this can't introduce new clipping on its own, only make
// the already-limited signal louder. Only other-engine/mix headroom is
// worth re-checking on real hardware if this ever gets played loud
// alongside Dexed/SampleSynth/Looper at once.
const EnsembleFactoryPreset kEnsembleFactoryPresets[] = {
    {"Warm Pad",        {0.560f, 0.015f, 0.245f, 0.015f, 0.105f, 0.010f, 0.050f}, 0.55f, 0.20f,
     0.85f, 0.30f, 0.90f, 0.70f, 0, 1.00f, 0.00f, 0.95f, 0.5f},
    {"Bright Ensemble", {0.240f, 0.135f, 0.205f, 0.135f, 0.145f, 0.090f, 0.050f}, 0.70f, 0.35f,
     0.40f, 0.30f, 0.85f, 0.50f, 0, 1.00f, 0.00f, 0.95f, 0.5f},
    {"Organ",           {0.400f, 0.075f, 0.225f, 0.075f, 0.125f, 0.050f, 0.050f}, 0.15f, 0.15f,
     0.02f, 0.10f, 1.00f, 0.10f, 0, 1.00f, 0.00f, 0.95f, 0.5f},
    // Thin Strings/Sub Swell/Mellow Choir's own LowPass cutoff01 bumped
    // up from their original values -- the project's own Cutoff curve
    // (kFilterMinHz=20/kFilterMaxHz=9000 in looper_layer.h, 20*450^x)
    // means cutoff01=0.35 (Sub Swell's original) lands at ~170Hz --
    // close to muting every harmonic above the fundamental for most
    // played notes, the real cause of "weak," not the drawbars
    // themselves (confirmed by computing the curve, not guessed).
    // Raised enough to stay clearly audible/characterful while still
    // reading as darker than the un-filtered presets. Bright Pluck's
    // sustain01 raised too -- 0.20 made a HELD note (not just a quick
    // tap) decay to near-silence and stay there, same "weak when held"
    // symptom with a different real cause; still decays audibly below
    // its own attack transient, just not to nothing. Output_level01
    // nudged up slightly on all four as a final, modest loudness match
    // against the un-filtered presets -- the drawbar registrations
    // themselves are untouched, so each preset's actual organ-stop
    // character is unchanged.
    {"Thin Strings",    {0.320f, 0.105f, 0.215f, 0.105f, 0.135f, 0.070f, 0.050f}, 0.45f, 0.25f,
     0.55f, 0.30f, 0.80f, 0.60f, 1, 0.75f, 0.10f, 1.00f, 0.5f},
    {"Sub Swell",       {0.520f, 0.030f, 0.240f, 0.030f, 0.110f, 0.020f, 0.050f}, 0.80f, 0.10f,
     0.75f, 0.40f, 0.95f, 0.85f, 1, 0.60f, 0.05f, 1.00f, 0.5f},
    {"Chorus Lead",     {0.360f, 0.090f, 0.220f, 0.090f, 0.130f, 0.060f, 0.050f}, 0.85f, 0.55f,
     0.10f, 0.25f, 0.75f, 0.35f, 0, 1.00f, 0.00f, 0.95f, 0.5f},
    {"Mellow Choir",    {0.480f, 0.045f, 0.235f, 0.045f, 0.115f, 0.030f, 0.050f}, 0.50f, 0.18f,
     0.70f, 0.35f, 0.90f, 0.75f, 1, 0.60f, 0.00f, 1.00f, 0.5f},
    {"Bright Pluck",    {0.280f, 0.120f, 0.210f, 0.120f, 0.140f, 0.080f, 0.050f}, 0.30f, 0.30f,
     0.01f, 0.20f, 0.40f, 0.25f, 0, 1.00f, 0.00f, 1.00f, 0.5f},
};
const int kEnsembleNumFactoryPresets
    = sizeof(kEnsembleFactoryPresets) / sizeof(kEnsembleFactoryPresets[0]);
