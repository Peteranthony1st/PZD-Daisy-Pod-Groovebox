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
// Gain and Level are matched so each is as loud as the Ensemble's New
// sound (Gain +3 dB, set by ear on the Pod) -- K-weighted loudness of a
// four-note chord at velocity 100; Glass Bells, which rings away, is left
// as it was.
const EnsembleFactoryPreset kEnsembleFactoryPresets[] = {
    // --- the original eight, re-voiced -------------------------------
    // Soft, fundamental-heavy pad: slow-ish swell, long tail, darkened
    // just enough to take the edge off, slow wide chorus.
    {"Warm Pad",        {0.85f, 0.10f, 0.45f, 0.00f, 0.15f, 0.00f, 0.05f}, 0.60f, 0.18f,
     0.72f, 0.60f, 0.90f, 0.82f, 1, 0.78f, 0.10f, 0.70f, 0.5f, 0.45f, 0.00f, 0.00f},
    // Classic bright string-machine section: 8'+4'+2' saws, deep chorus.
    {"Bright Ensemble", {0.75f, 0.00f, 0.65f, 0.15f, 0.45f, 0.10f, 0.20f}, 0.80f, 0.32f,
     0.55f, 0.50f, 0.90f, 0.70f, 0, 1.00f, 0.00f, 0.56f, 0.5f, 0.35f, 0.00f, 0.00f},
    // Hollow keys: squares for the hollow tone, instant attack and
    // release, a touch of slow chorus, a little room.
    {"Hollow Keys",     {0.20f, 0.85f, 0.10f, 0.65f, 0.00f, 0.40f, 0.20f}, 0.25f, 0.55f,
     0.05f, 0.30f, 1.00f, 0.25f, 0, 1.00f, 0.00f, 0.54f, 0.5f, 0.20f, 0.00f, 0.00f},
    // Airy upper strings: 4'/2' without much body, slower attack,
    // band of resonance at the top of the low-pass.
    {"Thin Strings",    {0.25f, 0.00f, 0.70f, 0.00f, 0.55f, 0.00f, 0.25f}, 0.70f, 0.25f,
     0.75f, 0.60f, 0.85f, 0.85f, 1, 0.82f, 0.25f, 0.65f, 0.5f, 0.50f, 0.00f, 0.00f},
    // Deep 8' swell that blooms over ~1.3s and rings on.
    {"Sub Swell",       {0.90f, 0.55f, 0.25f, 0.00f, 0.00f, 0.00f, 0.00f}, 0.70f, 0.12f,
     0.85f, 0.70f, 0.95f, 0.88f, 1, 0.62f, 0.20f, 0.57f, 0.5f, 0.55f, 0.00f, 0.00f},
    // Mono-style lead: fast attack, singing chorus vibrato, resonant
    // low-pass.
    {"Chorus Lead",     {0.80f, 0.30f, 0.55f, 0.00f, 0.25f, 0.00f, 0.10f}, 0.85f, 0.55f,
     0.25f, 0.55f, 0.85f, 0.55f, 1, 0.80f, 0.40f, 0.58f, 0.5f, 0.25f, 0.00f, 0.00f},
    // Soft "aah" choir: squares + 4' saw through a gentle band-pass
    // for a vocal formant, slow swell, generous hall.
    {"Mellow Choir",    {0.55f, 0.60f, 0.40f, 0.30f, 0.10f, 0.00f, 0.00f}, 0.65f, 0.20f,
     0.78f, 0.60f, 0.90f, 0.85f, 3, 0.68f, 0.30f, 0.73f, 0.5f, 0.60f, 0.00f, 0.00f},
    // Percussive pluck: instant attack, short decay onto a low
    // sustain, bright filter with a little bite, a little room.
    {"Bright Pluck",    {0.70f, 0.00f, 0.60f, 0.20f, 0.40f, 0.10f, 0.30f}, 0.35f, 0.40f,
     0.02f, 0.62f, 0.25f, 0.62f, 1, 0.88f, 0.30f, 0.85f, 0.5f, 0.30f, 0.00f, 0.14f},

    // --- new ---------------------------------------------------------
    // The ARP Solina sound: 8'+4' saws, very deep, slow-ish ensemble
    // chorus, half-second bloom, long release.
    {"Solina Strings",  {0.85f, 0.00f, 0.75f, 0.00f, 0.20f, 0.00f, 0.00f}, 0.95f, 0.28f,
     0.70f, 0.60f, 0.92f, 0.85f, 1, 0.86f, 0.10f, 0.59f, 0.5f, 0.45f, 0.00f, 0.00f},
    // Bell / glass: bright upper squares and saws, instant strike, a
    // ~1.3s decay to nothing, long ring-out.
    {"Glass Bells",     {0.10f, 0.00f, 0.25f, 0.70f, 0.55f, 0.65f, 0.50f}, 0.40f, 0.60f,
     0.00f, 0.85f, 0.00f, 0.82f, 2, 0.30f, 0.10f, 0.85f, 0.5f, 0.55f, 0.00f, 0.19f},
    // Short synth stab for chords on the beat: everything open, snappy
    // decay, resonant low-pass for punch.
    {"Poly Stab",       {0.80f, 0.40f, 0.60f, 0.30f, 0.45f, 0.20f, 0.30f}, 0.30f, 0.45f,
     0.00f, 0.50f, 0.30f, 0.40f, 1, 0.80f, 0.45f, 0.74f, 0.5f, 0.25f, 0.00f, 0.00f},
    // Dark, slowly breathing drone: ~3s fade-in, 4s fade-out, low
    // cutoff with resonance, slowest deepest chorus, big hall.
    {"Dark Drone",      {0.85f, 0.75f, 0.20f, 0.10f, 0.00f, 0.00f, 0.00f}, 0.90f, 0.05f,
     0.95f, 0.80f, 1.00f, 1.00f, 1, 0.55f, 0.45f, 0.48f, 0.5f, 0.65f, 0.00f, 0.00f},
    // Nasal reed: square-heavy registration, firm attack, almost no
    // chorus, a slightly nasal band-pass.
    {"Nasal Reed",      {0.30f, 0.90f, 0.15f, 0.55f, 0.00f, 0.25f, 0.00f}, 0.10f, 0.40f,
     0.35f, 0.40f, 0.95f, 0.45f, 3, 0.80f, 0.20f, 0.70f, 0.5f, 0.30f, 0.00f, 0.00f},
    // Shimmering high pad: only the upper registers, high-passed so it
    // floats above a mix, fastest chorus, lots of reverb.
    {"Shimmer Pad",     {0.00f, 0.00f, 0.35f, 0.20f, 0.70f, 0.30f, 0.60f}, 1.00f, 0.60f,
     0.80f, 0.65f, 0.90f, 0.92f, 2, 0.45f, 0.15f, 0.57f, 0.5f, 0.70f, 0.00f, 0.00f},
    // Brass-like swell: bright saws with a ~0.3s attack and a resonant
    // low-pass that gives it the "blat", light chorus.
    {"Brass Swell",     {0.90f, 0.20f, 0.70f, 0.10f, 0.45f, 0.00f, 0.25f}, 0.30f, 0.35f,
     0.65f, 0.55f, 0.85f, 0.55f, 1, 0.72f, 0.50f, 0.51f, 0.5f, 0.30f, 0.00f, 0.00f},
    // Round sub bass: just the 8' saw and square, dark low-pass, tight
    // envelope, dry -- for bass lines under everything else.
    {"Sub Bass",        {0.90f, 0.70f, 0.10f, 0.00f, 0.00f, 0.00f, 0.00f}, 0.00f, 0.20f,
     0.10f, 0.45f, 0.85f, 0.35f, 1, 0.55f, 0.30f, 0.59f, 0.5f, 0.00f, 0.00f, 0.00f},

    // --- made for the LFO (see kEnsembleFactoryExtras) ----------------
    // Solina strings chopped in 1/16s by the LFO -- the trance gate.
    {"Gated Strings",   {0.85f, 0.00f, 0.75f, 0.00f, 0.20f, 0.00f, 0.00f}, 0.80f, 0.28f,
     0.00f, 0.50f, 1.00f, 0.40f, 1, 0.86f, 0.10f, 0.67f, 0.5f, 0.45f, 0.00f, 0.00f},
    // Hollow keys with a steady tremolo.
    {"Tremolo Keys",    {0.20f, 0.85f, 0.10f, 0.65f, 0.00f, 0.40f, 0.20f}, 0.25f, 0.55f,
     0.05f, 0.30f, 1.00f, 0.30f, 0, 1.00f, 0.00f, 0.58f, 0.5f, 0.30f, 0.00f, 0.00f},
    // A warm pad pulsing in 1/8s.
    {"Pulse Pad",       {0.85f, 0.10f, 0.45f, 0.00f, 0.15f, 0.00f, 0.05f}, 0.60f, 0.18f,
     0.10f, 0.60f, 0.95f, 0.70f, 1, 0.78f, 0.10f, 0.73f, 0.5f, 0.45f, 0.00f, 0.00f},
};

// {vel, wave (0 Tri, 2 Square), rate, sync, sync note (2 = 1/16, 4 = 1/8),
//  fade, pitch, filter, amp} -- velocity most on the keys and plucks, none
// on Solina Strings (a string machine's keys never were); vibrato fading
// in on the strings, choir and leads; slow breathing on the drones.
const EnsembleFactoryExtra kEnsembleFactoryExtras[] = {
    {0.30f, 0, 0.45f, 0, 2, 0.55f, 0.55f, 0.50f, 0.50f}, // Warm Pad
    {0.35f, 0, 0.40f, 0, 2, 0.00f, 0.50f, 0.50f, 0.50f}, // Bright Ensemble
    {0.70f, 0, 0.40f, 0, 2, 0.00f, 0.50f, 0.50f, 0.50f}, // Hollow Keys
    {0.35f, 0, 0.50f, 0, 2, 0.60f, 0.55f, 0.50f, 0.50f}, // Thin Strings
    {0.30f, 0, 0.15f, 0, 2, 0.00f, 0.50f, 0.62f, 0.50f}, // Sub Swell
    {0.60f, 0, 0.55f, 0, 2, 0.50f, 0.56f, 0.50f, 0.50f}, // Chorus Lead
    {0.30f, 0, 0.48f, 0, 2, 0.60f, 0.55f, 0.50f, 0.50f}, // Mellow Choir
    {0.80f, 0, 0.40f, 0, 2, 0.00f, 0.50f, 0.50f, 0.50f}, // Bright Pluck
    {0.00f, 0, 0.40f, 0, 2, 0.00f, 0.50f, 0.50f, 0.50f}, // Solina Strings
    {0.70f, 0, 0.40f, 0, 2, 0.00f, 0.50f, 0.50f, 0.50f}, // Glass Bells
    {0.75f, 0, 0.40f, 0, 2, 0.00f, 0.50f, 0.50f, 0.50f}, // Poly Stab
    {0.00f, 0, 0.10f, 0, 2, 0.00f, 0.50f, 0.68f, 0.50f}, // Dark Drone
    {0.45f, 0, 0.50f, 0, 2, 0.40f, 0.55f, 0.50f, 0.50f}, // Nasal Reed
    {0.25f, 0, 0.50f, 0, 2, 0.00f, 0.50f, 0.50f, 0.62f}, // Shimmer Pad
    {0.70f, 0, 0.40f, 0, 2, 0.00f, 0.50f, 0.50f, 0.50f}, // Brass Swell
    {0.50f, 0, 0.40f, 0, 2, 0.00f, 0.50f, 0.50f, 0.50f}, // Sub Bass
    {0.30f, 2, 0.40f, 1, 2, 0.00f, 0.50f, 0.50f, 1.00f}, // Gated Strings
    {0.70f, 0, 0.55f, 0, 2, 0.00f, 0.50f, 0.50f, 0.70f}, // Tremolo Keys
    {0.30f, 2, 0.40f, 1, 4, 0.00f, 0.50f, 0.50f, 0.92f}, // Pulse Pad
};
static_assert(sizeof(kEnsembleFactoryExtras) / sizeof(kEnsembleFactoryExtras[0])
                  == sizeof(kEnsembleFactoryPresets) / sizeof(kEnsembleFactoryPresets[0]),
              "one kEnsembleFactoryExtras entry per Ensemble factory preset");
const int kEnsembleNumFactoryPresets
    = sizeof(kEnsembleFactoryPresets) / sizeof(kEnsembleFactoryPresets[0]);

// The Organ's bank -- classic drawbar registrations (written as the
// usual 9-digit settings, each digit / 8), instant attack, a short
// release, a little chorus and room. Gain does the loudness: thin
// registrations get more, full ones less, and Rock Drive pushes the
// limiter into overdrive on purpose.
#define DB(a, b, c, d, e, f, g, h, i) \
    {a / 8.f, b / 8.f, c / 8.f, d / 8.f, e / 8.f, f / 8.f, g / 8.f, h / 8.f, i / 8.f}
const OrganFactoryPreset kOrganFactoryPresets[] = {
    // {name, drawbars, chorus depth/rate, A/D/S/R, filter mode/cutoff/res,
    //  level, pan, reverb, delay, gain}
    {"Jazz 888",    DB(8, 8, 8, 0, 0, 0, 0, 0, 0), 0.20f, 0.30f,
     0.05f, 0.30f, 1.00f, 0.25f, 0, 1.00f, 0.00f, 0.85f, 0.5f, 0.25f, 0.00f, 0.50f},
    {"Full Organ",  DB(8, 8, 8, 8, 8, 8, 8, 8, 8), 0.20f, 0.30f,
     0.05f, 0.30f, 1.00f, 0.25f, 0, 1.00f, 0.00f, 0.85f, 0.5f, 0.30f, 0.00f, 0.10f},
    {"Soul 8888",   DB(8, 8, 8, 8, 0, 0, 0, 0, 0), 0.25f, 0.35f,
     0.05f, 0.30f, 1.00f, 0.25f, 0, 1.00f, 0.00f, 0.85f, 0.5f, 0.25f, 0.00f, 0.45f},
    {"Gospel",      DB(8, 8, 8, 8, 0, 8, 0, 0, 8), 0.35f, 0.50f,
     0.05f, 0.30f, 1.00f, 0.30f, 0, 1.00f, 0.00f, 0.85f, 0.5f, 0.35f, 0.00f, 0.35f},
    {"Ballad",      DB(0, 0, 8, 6, 0, 0, 0, 0, 0), 0.30f, 0.25f,
     0.10f, 0.30f, 1.00f, 0.35f, 0, 1.00f, 0.00f, 0.85f, 0.5f, 0.45f, 0.00f, 0.70f},
    {"Flute 8'",    DB(0, 0, 8, 0, 0, 0, 0, 0, 0), 0.10f, 0.25f,
     0.10f, 0.30f, 1.00f, 0.30f, 0, 1.00f, 0.00f, 0.85f, 0.5f, 0.35f, 0.00f, 0.90f},
    {"Rock Drive",  DB(8, 8, 8, 6, 0, 0, 0, 0, 0), 0.25f, 0.45f,
     0.05f, 0.30f, 1.00f, 0.25f, 0, 1.00f, 0.00f, 0.80f, 0.5f, 0.20f, 0.00f, 1.00f},
    {"Cathedral",   DB(8, 6, 8, 6, 5, 4, 3, 0, 0), 0.15f, 0.20f,
     0.30f, 0.30f, 1.00f, 0.80f, 0, 1.00f, 0.00f, 0.85f, 0.5f, 0.80f, 0.00f, 0.30f},
};
#undef DB
const int kOrganNumFactoryPresets = sizeof(kOrganFactoryPresets) / sizeof(kOrganFactoryPresets[0]);
