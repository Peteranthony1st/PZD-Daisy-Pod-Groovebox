#pragma once
#include <cstdint>

// An arpeggiator per synth engine (main.cpp's SustainEngine order), all on
// one clock. Two modes:
//
//   Generator -- the key pressed builds a pool: the arp's scale from that
//     note, Range octaves of it. The arp walks the pool: Steps long, moving
//     Jump notes at a time in a Shape (up, up/down, 3 forward 2 back...),
//     each note played Repeat times. No stored patterns -- four controls
//     and the scale make the melody, so a new scale is a new tune.
//   Phrase -- a pre-programmed phrase plays from the key pressed: written
//     in scale steps (so it's in the arp's scale, and the scale colours
//     it), each step with its own velocity, length, octave, chance and
//     ratchet.
//
// Hold two or more keys and the arp plays that chord instead (over Range
// octaves): Follow works out the chord in any inversion (E G C is C major)
// and plays it from its root; Lock plays the notes held, moved onto the
// scale. As on other arps, a new chord is taken at the next step: the note
// playing is never cut off or played again. A step plays with whatever keys
// are down when it's due; once they've stopped changing for a moment (a
// strum, kSettleMs) the chord's taken for good, for the steps after. With
// Latch only pressing keys changes the chord -- letting go never does. One key: Chord picks which scale notes make the
// pool: a Triad, 7th or Power chord
// built in the scale on the note pressed (C major, Lock: F plays F A C, G
// plays G B D, A plays A C E), or the whole Scale for runs.
//
// Follow: the key pressed is the new key (the scale moves with it). Lock:
// the key stays put and the note pressed only says where in it to start,
// so D in C major plays D Dorian-ish, G plays G Mixolydian-ish -- chord
// changes with one finger. Either way a new key takes over on the next
// step. Latch keeps it playing after the key's let go.
//
// Variation 1..99 picks a fixed flavour -- accents, short and long notes,
// octave jumps, the odd rest and ratchet per step -- the same every time
// (so it saves and exports the same); 0 is straight.
//
// Arps: one per engine (kEngines, for playing live -- Global:Arp), then one
// per MIDI track (TrackArp(t)), which plays the track's notes and the keys
// while the track's selected. A track's arp settings are per pattern: the
// pattern playing picks them. A pattern never set up takes the settings of
// the one last edited the first time it's played or opened, so a track
// feels like one arp until a pattern's made different.
//
// Threads: Tick(), KeyDown() and KeyUp() run in main.cpp's 1kHz control
// interrupt (with the live MIDI and the sequencer, so a note from an arp
// can't land in the middle of an engine's own voice allocation). The UI
// only writes settings.
class Arpeggiator
{
  public:
    static constexpr int kEngines  = 7;  // main.cpp's SustainEngine
    static constexpr int kTracks   = 8;  // MidiSequencer::kTracks
    static constexpr int kPatterns = 8;  // MidiSequencer::kPatterns
    static constexpr int kInstances = kEngines + kTracks;
    static constexpr int TrackArp(int t) { return kEngines + t; }
    static constexpr int kPpq      = 96; // ticks per beat, as the sequencer

    enum Mode : uint8_t
    {
        kGenerator,
        kPhrase,
        kNumModes
    };

    // --- Phrase mode ---------------------------------------------------
    // A phrase step: a scale step from the root as in a 7-note scale (0 the
    // root, 2 the third, 4 the fifth, 7 the octave -- see PhraseNote()), or
    // a rest or a tie (the note before carries on through it); then its
    // velocity (0 ghost, 1 soft, 2 normal, 3 accent), length (0 short,
    // 1 normal, 2 long -- the whole step), octave (-1..+1), chance (1: plays
    // half the time) and ratchet (1..3 hits in the step).
    static constexpr int8_t kRest = -128, kTie = -127;
    static constexpr int    kMaxPhraseSteps = 16;
    struct Step
    {
        int8_t  degree;
        uint8_t velocity, length;
        int8_t  octave;
        uint8_t chance, ratchet;
    };
    struct Phrase
    {
        const char* name; // up to 7 letters (the Phrase page's footer)
        int         len;
        Step        steps[kMaxPhraseSteps];
    };
    static constexpr int kNumPhrases = 24;
    static const Phrase& GetPhrase(int i);

    // --- Generator mode ------------------------------------------------
    static constexpr int kMaxGenSteps = 16;
    static constexpr int kMaxJump     = 7;
    static constexpr int kNumShapes   = 9;
    static constexpr int kMaxRepeat   = 4;
    static constexpr int kMaxRange    = 3;  // octaves
    static constexpr int kMaxPool     = 40; // 3 octaves of a 12-note scale + 1
    static const char*   ShapeName(int s);  // "Up", "3+2-" ...
    // Saved by number, so new ones go on the end; NextChord() steps them in
    // a sensible order (Triad, 7th, Sus2, Sus4, Power, Scale).
    enum Chord : uint8_t
    {
        kTriad,
        kSeventh,
        kPower,
        kScaleRun,
        kSus2,
        kSus4,
        kNumChords
    };
    static const char* ChordName(int c); // "Triad" ...
    static uint8_t     NextChord(int c);

    // --- Feel ------------------------------------------------------------
    static constexpr int kNumVariations = 100; // 0 = straight
    struct Flavour
    {
        int   velocity; // added to the key's
        float gate;     // times Gate
        int   octave;   // semitones added: 0, +12, -12
        bool  rest;
        int   ratchet;  // plays per step: 1..3
    };
    static Flavour StepFlavour(int variation, int k);

    enum Rate : uint8_t
    {
        kRate4,
        kRate8,
        kRate8T,
        kRate16,
        kRate16T,
        kRate32,
        kNumRates
    };
    static const char* RateName(int r); // "1/4" ...
    static constexpr int kNumScales = 8;
    static const char* ScaleName(int s); // "Major" ...
    static uint16_t    ScaleMask(int s); // bit n: n semitones above the key is in it
    static int         ScaleLen(int s);  // notes in an octave of it
    static const char* KeyName(int k);   // "C" ...

    struct Settings
    {
        bool    on      = false;
        uint8_t mode    = kGenerator;
        uint8_t rate    = kRate16;
        bool    latch   = true;
        // Generator
        uint8_t steps   = 8;
        uint8_t jump    = 1;
        uint8_t shape   = 0;
        uint8_t repeat  = 1;
        // Phrase
        uint8_t phrase  = 0;
        int8_t  octave  = 0; // the phrase up or down, -2..+2
        // Feel
        float   gate01    = 0.6f; // of the step
        float   swing01   = 0.f;  // delays every second step by up to half a step
        uint8_t variation = 0;
        // Key
        uint8_t key   = 0; // 0 = C
        uint8_t scale = 0; // see ScaleName()
        bool    lock  = false; // false = Follow
        uint8_t range = 2;     // octaves, 1..kMaxRange
        uint8_t chord = kTriad;
    };
    // The pool for a key pressed (lowest first); returns its size.
    static int PoolNotes(const Settings& s, int from, int* out);
    // Which pool note step k of the pattern plays (0..n-1).
    static int GenIndex(const Settings& s, int n, int k);
    // A phrase step's note from the key played (before the step's and the
    // phrase's octave): Follow -- `degree` steps up the scale from it; Lock
    // -- up the arp's own scale from the scale note nearest it. Steps count
    // as in a 7-note scale: in a 5- or 6-note one each lands on the scale's
    // note nearest that step's place in a major scale, so an octave stays an
    // octave and a fifth a fifth.
    static int PhraseNote(const Settings& s, int root, int degree);

    // An arp's settings: an engine's own, or a track's playing pattern's.
    Settings&       Get(int i) { return i < kEngines ? settings_[i] : pattern_[i - kEngines][cur_[i - kEngines]]; }
    const Settings& Get(int i) const
    {
        return i < kEngines ? settings_[i] : pattern_[i - kEngines][cur_[i - kEngines]];
    }
    bool            IsOn(int i) const { return Get(i).on; }
    // Track t's pattern p's settings, to edit (UI): one never set up takes
    // the last edited's first.
    Settings&       PatternSettings(int t, int p);
    // Without taking anything over (for drawing a summary).
    const Settings& PeekPattern(int t, int p) const
    {
        return own_[t][p] ? pattern_[t][p] : pattern_[t][last_[t]];
    }
    void CopyPattern(int t, int from, int to); // a pattern copied, its arp too
    void CopyToAllPatterns(int t, int from);
    // Control interrupt: the pattern track t is playing (its arp's
    // settings); a change silences the arp, to start afresh.
    void SetPlayingPattern(int t, int p);
    // Control interrupt: track t is back at its top -- its arp's pattern
    // starts over with it, so every loop plays the same (a Steps that
    // doesn't divide the loop would otherwise start each one somewhere
    // else).
    void TrackLoopStart(int t);
    // Defaults everywhere (all off), every arp silenced (a performance
    // without arps loading). Main loop: the silencing is left to Tick(), so
    // a note off never lands mid-way through an engine's voice allocation.
    void Reset();
    // Saving with a performance: every engine's settings, then every
    // track's patterns', kSaveEntries of SaveEntry.
    struct SaveEntry
    {
        uint8_t on, mode, rate, latch, steps, jump, shape, repeat;
        uint8_t phrase, variation, key, scale, lock, range, chord, own;
        float   gate01, swing01;
        int8_t  octave; // the phrase's
        uint8_t reserved[7];
    };
    static_assert(sizeof(SaveEntry) == 32, "SaveEntry is a file format: append inside reserved[]");
    static constexpr int kSaveEntries = kEngines + kTracks * kPatterns;
    void Save(SaveEntry* out) const;
    void Load(const SaveEntry* in, int n);

    // --- Rendering (main loop) -----------------------------------------
    // What a track's arp plays over a pattern, worked out offline -- for
    // drawing it on the MIDI grid and printing it into notes. `ev` is one
    // lap of the track's playback (tick order, offs before ons at a shared
    // tick, as the sequencer plays it) of `len` ticks; the notes of the
    // third lap come back (the first two settle latch and the step count,
    // as a track that's been playing). This object is used as scratch: give
    // it one of its own, not the live arps.
    struct KeyEvent
    {
        uint32_t tick;
        uint8_t  note, velocity; // velocity 0 = key up
    };
    struct RenderedNote
    {
        uint32_t tick, length; // length may run past the end (it wraps)
        uint8_t  note, velocity;
    };
    int Render(const Settings& s, const KeyEvent* ev, int n, uint32_t len, RenderedNote* out, int max);
    // For drawing: the step last played (-1 = stopped) and the key the
    // arp plays from (-1 = none yet).
    int LastStep(int e) const { return arps_[e].active ? arps_[e].last_step : -1; }
    int Root(int e) const { return arps_[e].root; }
    // The pool engine e's Generator is playing (held chord or one-key
    // chord); 0 when it has no key yet. Whether it's a held chord.
    int  CurrentPool(int e, int* out) const;
    bool HoldingChord(int e) const { return arps_[e].chord_count >= 2; }
    // The held chord's name for display ("Am7", "F/C"...); "" if none.
    void HeldChordName(int e, char* out, int size) const;

    using NoteFn = void (*)(int engine, uint8_t note, uint8_t velocity); // vel 0 = off
    void Init(NoteFn fn) { note_fn_ = fn; }

    // --- Control interrupt ------------------------------------------------
    // at_once: the keys come together, no strum to wait out (a track's
    // notes, which land on the same tick).
    void KeyDown(int e, uint8_t note, uint8_t velocity, bool at_once = false);
    void KeyUp(int e, uint8_t note, bool at_once = false);
    // abs_tick: ticks since the clock started, never wrapping (the loop
    // position when the looper runs, so steps land on the beat grid);
    // running: it's advancing; now_ms: the time (for the strum window).
    void Tick(uint64_t abs_tick, bool running, uint32_t now_ms);
    void Stop(int e);    // silence it and forget its keys (off, panic)
    void StopAll();

  private:
    struct State
    {
        bool     held[128]   = {};
        int      held_count  = 0;
        // The chord the arp plays, taken kSettleMs after the keys stop
        // changing: the keys down -- with Latch, the keys down as the last
        // one was pressed (so letting go, finger by finger, changes
        // nothing).
        bool     chord[128]   = {};
        int      chord_count  = 0;
        bool     struck[128]  = {}; // the keys down as of the last press
        int      struck_count = 0;
        bool     pressed      = false; // a key's gone down since the chord was taken
        uint32_t changed_ms   = 0;
        bool     settled      = true;
        // Starting, as other arps synced to a clock: on the first step at or
        // after the key went down -- never part way through one, and never
        // on one just played (stopped and started again within a step).
        bool     starting     = false;
        uint64_t press_tick   = 0;
        uint64_t start_at     = 0;
        bool     start_known  = false;
        int      root        = -1; // the key the arp plays from
        uint8_t  velocity    = 100;
        bool     active      = false;
        int      step        = 0;  // the next step to play
        int      last_step   = -1;
        uint32_t cycle       = 0;  // times round the pattern (a chance step's dice)
        int      last_index  = -1; // the pool note last played (Generator)
        int      index_before = -1; // ... and the one before this step's
        uint32_t change_ms   = 0;  // ... in ms
        uint32_t down_ms[128] = {}; // when each key went down
        uint8_t  rate_used   = 0xFF; // the rate the steps are on (a change waits for its grid)
        int64_t  last_slot   = -1; // the step slot last played
        int      sounding    = -1; // note on now
        uint64_t off_at      = 0;  // tick its gate ends
        // A ratchet's repeats still to come within the step.
        int      ratchet_left  = 0;
        uint64_t ratchet_next  = 0;
        int      ratchet_every = 0;
        int      ratchet_len   = 0;
        uint8_t  ratchet_vel   = 0;
        int      ratchet_note  = -1;
    };
    int  StepTicks(const Settings& s) const;
    void NoteOff(int e);
    void NoteOn(int e, int note, int velocity, uint64_t now, int len);
    // Follow's chord finder: the root (0..11) and quality (an index into
    // the chord table) of a set of pitch classes; false if no match.
    static bool FindChord(uint16_t pcs, int* root, int* quality);
    // The keys down become the chord; true if it took one. final: the strum
    // has settled (else just for the step due now -- it's taken again when
    // it settles).
    bool        TakeChord(int e, bool final = true);
    // Plays step k of engine e's arp now (a step of st ticks).
    void        PlayStep(int e, int k, uint64_t abs_tick, int st);
    static constexpr uint32_t kSettleMs = 40;
    uint32_t ms_ = 0; // the time, as of the last Tick()
    uint64_t now_tick_ = 0; // the clock, as of the last Tick()

    Settings settings_[kEngines];
    Settings pattern_[kTracks][kPatterns];
    bool     own_[kTracks][kPatterns] = {}; // set up (else the last edited's)
    uint8_t  last_[kTracks]           = {}; // the pattern last edited
    uint8_t  cur_[kTracks]            = {}; // the pattern playing
    void     Inherit(int t, int p);
    volatile bool stop_all_ = false; // Reset() asked Tick() to silence everything
    State    arps_[kInstances];
    NoteFn   note_fn_ = nullptr;
};
