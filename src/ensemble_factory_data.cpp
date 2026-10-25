#include "ensemble_factory_data.h"

// The factory bank -- re-voiced as a whole (v3.3), each preset now using
// every part of the engine: registration, envelope, filter, chorus, Gain
// into the limiter, and its own reverb send.
//
// Reference for the numbers below (so they can be tuned by ear):
//   drawbars    0=Saw8' 1=Sq8' 2=Saw4' 3=Sq4' 4=Saw2' 5=Sq2' 6=Saw1'
//   chorus rate 0.05 + 1.95*r Hz          (0.2 = 0.44Hz, 0.5 = 1.0Hz)
//   A/D/R       0.002 * 2000^x seconds    (0.5 = 90ms, 0.7 = 0.4s,
//                                          0.8 = 0.9s, 0.9 = 1.9s)
//   cutoff      20 * 450^x Hz             (0.6 = 0.8k, 0.75 = 2k,
//                                          0.85 = 3.6k, 1.0 = 9k)
//   filter_mode 0=Off 1=LowPass 2=HighPass 3=BandPass
//   gain01      4^x into the limiter      (0.5 = +6dB, 1.0 = +12dB)
// Output Level sits at 0.85 throughout (0.93x after the limiter) and
// Gain sets the loudness instead -- thin registrations and single-note
// leads get more, full chords less. Each preset sends to the shared
// reverb (Size is global, Global:Reverb); none use the delay -- left
// for you to add per song, since echoes on these long-release sounds
// just smear.
//
// {name, {drawbars x7}, chorus_depth01, chorus_rate01,
//  attack01, decay01, sustain01, release01,
//  filter_mode, filter_cutoff01, filter_res01, output_level01, pan01,
//  reverb_send01, delay_send01, gain01}
const EnsembleFactoryPreset kEnsembleFactoryPresets[] = {
    // --- the original eight, re-voiced -------------------------------
    // Soft, fundamental-heavy pad: slow-ish swell, long tail, darkened
    // just enough to take the edge off, slow wide chorus.
    {"Warm Pad",        {0.85f, 0.10f, 0.45f, 0.00f, 0.15f, 0.00f, 0.05f}, 0.60f, 0.18f,
     0.72f, 0.60f, 0.90f, 0.82f, 1, 0.78f, 0.10f, 0.85f, 0.5f, 0.45f, 0.00f, 0.55f},
    // Classic bright string-machine section: 8'+4'+2' saws, deep chorus.
    {"Bright Ensemble", {0.75f, 0.00f, 0.65f, 0.15f, 0.45f, 0.10f, 0.20f}, 0.80f, 0.32f,
     0.55f, 0.50f, 0.90f, 0.70f, 0, 1.00f, 0.00f, 0.85f, 0.5f, 0.35f, 0.00f, 0.40f},
    // Drawbar organ: squares for the hollow tone, instant attack and
    // release, a touch of slow chorus as "leslie", a little room.
    {"Organ",           {0.20f, 0.85f, 0.10f, 0.65f, 0.00f, 0.40f, 0.20f}, 0.25f, 0.55f,
     0.05f, 0.30f, 1.00f, 0.25f, 0, 1.00f, 0.00f, 0.85f, 0.5f, 0.20f, 0.00f, 0.45f},
    // Airy upper strings: 4'/2' without much body, slower attack,
    // band of resonance at the top of the low-pass.
    {"Thin Strings",    {0.25f, 0.00f, 0.70f, 0.00f, 0.55f, 0.00f, 0.25f}, 0.70f, 0.25f,
     0.75f, 0.60f, 0.85f, 0.85f, 1, 0.82f, 0.25f, 0.85f, 0.5f, 0.50f, 0.00f, 0.35f},
    // Deep 8' swell that blooms over ~1.3s and rings on.
    {"Sub Swell",       {0.90f, 0.55f, 0.25f, 0.00f, 0.00f, 0.00f, 0.00f}, 0.70f, 0.12f,
     0.85f, 0.70f, 0.95f, 0.88f, 1, 0.62f, 0.20f, 0.85f, 0.5f, 0.55f, 0.00f, 0.45f},
    // Mono-style lead: fast attack, singing chorus vibrato, resonant
    // low-pass.
    {"Chorus Lead",     {0.80f, 0.30f, 0.55f, 0.00f, 0.25f, 0.00f, 0.10f}, 0.85f, 0.55f,
     0.25f, 0.55f, 0.85f, 0.55f, 1, 0.80f, 0.40f, 0.85f, 0.5f, 0.25f, 0.00f, 0.80f},
    // Soft "aah" choir: squares + 4' saw through a gentle band-pass
    // for a vocal formant, slow swell, generous hall.
    {"Mellow Choir",    {0.55f, 0.60f, 0.40f, 0.30f, 0.10f, 0.00f, 0.00f}, 0.65f, 0.20f,
     0.78f, 0.60f, 0.90f, 0.85f, 3, 0.68f, 0.30f, 0.85f, 0.5f, 0.60f, 0.00f, 0.75f},
    // Percussive pluck: instant attack, short decay onto a low
    // sustain, bright filter with a little bite, a little room.
    {"Bright Pluck",    {0.70f, 0.00f, 0.60f, 0.20f, 0.40f, 0.10f, 0.30f}, 0.35f, 0.40f,
     0.02f, 0.62f, 0.25f, 0.62f, 1, 0.88f, 0.30f, 0.85f, 0.5f, 0.30f, 0.00f, 0.55f},

    // --- new ---------------------------------------------------------
    // The ARP Solina sound: 8'+4' saws, very deep, slow-ish ensemble
    // chorus, half-second bloom, long release.
    {"Solina Strings",  {0.85f, 0.00f, 0.75f, 0.00f, 0.20f, 0.00f, 0.00f}, 0.95f, 0.28f,
     0.70f, 0.60f, 0.92f, 0.85f, 1, 0.86f, 0.10f, 0.85f, 0.5f, 0.45f, 0.00f, 0.50f},
    // Bell / glass: bright upper squares and saws, instant strike, a
    // ~1.3s decay to nothing, long ring-out.
    {"Glass Bells",     {0.10f, 0.00f, 0.25f, 0.70f, 0.55f, 0.65f, 0.50f}, 0.40f, 0.60f,
     0.00f, 0.85f, 0.00f, 0.82f, 2, 0.30f, 0.10f, 0.85f, 0.5f, 0.55f, 0.00f, 0.40f},
    // Short synth stab for chords on the beat: everything open, snappy
    // decay, resonant low-pass for punch.
    {"Poly Stab",       {0.80f, 0.40f, 0.60f, 0.30f, 0.45f, 0.20f, 0.30f}, 0.30f, 0.45f,
     0.00f, 0.50f, 0.30f, 0.40f, 1, 0.80f, 0.45f, 0.85f, 0.5f, 0.25f, 0.00f, 0.35f},
    // Dark, slowly breathing drone: ~3s fade-in, 4s fade-out, low
    // cutoff with resonance, slowest deepest chorus, big hall.
    {"Dark Drone",      {0.85f, 0.75f, 0.20f, 0.10f, 0.00f, 0.00f, 0.00f}, 0.90f, 0.05f,
     0.95f, 0.80f, 1.00f, 1.00f, 1, 0.55f, 0.45f, 0.85f, 0.5f, 0.65f, 0.00f, 0.55f},
    // Reed / harmonium: square-heavy registration, firm attack, almost
    // no chorus, a slightly nasal band-pass.
    {"Reed Organ",      {0.30f, 0.90f, 0.15f, 0.55f, 0.00f, 0.25f, 0.00f}, 0.10f, 0.40f,
     0.35f, 0.40f, 0.95f, 0.45f, 3, 0.80f, 0.20f, 0.85f, 0.5f, 0.30f, 0.00f, 0.65f},
    // Shimmering high pad: only the upper registers, high-passed so it
    // floats above a mix, fastest chorus, lots of reverb.
    {"Shimmer Pad",     {0.00f, 0.00f, 0.35f, 0.20f, 0.70f, 0.30f, 0.60f}, 1.00f, 0.60f,
     0.80f, 0.65f, 0.90f, 0.92f, 2, 0.45f, 0.15f, 0.85f, 0.5f, 0.70f, 0.00f, 0.20f},
    // Brass-like swell: bright saws with a ~0.3s attack and a resonant
    // low-pass that gives it the "blat", light chorus.
    {"Brass Swell",     {0.90f, 0.20f, 0.70f, 0.10f, 0.45f, 0.00f, 0.25f}, 0.30f, 0.35f,
     0.65f, 0.55f, 0.85f, 0.55f, 1, 0.72f, 0.50f, 0.85f, 0.5f, 0.30f, 0.00f, 0.45f},
    // Round sub bass: just the 8' saw and square, dark low-pass, tight
    // envelope, dry -- for bass lines under everything else.
    {"Sub Bass",        {0.90f, 0.70f, 0.10f, 0.00f, 0.00f, 0.00f, 0.00f}, 0.00f, 0.20f,
     0.10f, 0.45f, 0.85f, 0.35f, 1, 0.55f, 0.30f, 0.85f, 0.5f, 0.00f, 0.00f, 0.90f},
};
const int kEnsembleNumFactoryPresets
    = sizeof(kEnsembleFactoryPresets) / sizeof(kEnsembleFactoryPresets[0]);
