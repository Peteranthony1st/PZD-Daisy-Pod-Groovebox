#pragma once
#include <cstddef>
#include <cstdint>
#include <climits>

// The MIDI sequencer: 8 tracks of recorded notes that play the synth
// engines in time with the looper's tempo clock -- Home's M1..M8, after
// the four audio layers.
//
// Threads. Everything that touches notes (Tick(), OnLiveNote()) runs in
// main.cpp's 1kHz control interrupt, the same context that already
// dispatches live MIDI, so a sequenced NoteOn can never interrupt a live
// one halfway through an engine's voice allocation. The UI (main loop)
// only writes plain settings (engine, length, mute) and raises Request*()
// flags, which Tick() acts on; it reads state back for drawing.
//
// Time. One tick = 1/kPpq of a beat. main.cpp's audio callback publishes
// the loop position (in ticks) each block; Tick() works out each track's
// own position from it -- a track can be shorter than the loop (it
// repeats inside it) or a whole multiple of it (it counts loop wraps).
class MidiSequencer
{
  public:
    static constexpr int      kTracks    = 8;
    static constexpr int      kMaxEvents = 1024; // per track
    // The playback copy has room for rolls to add their repeats.
    static constexpr int      kMaxPlayEvents = 1536;
    static constexpr uint32_t kPpq       = 96;   // ticks per beat
    static constexpr int      kMaxBars   = 16;   // a track never runs longer
    // A note played up to this early, while a track counts in, is moved
    // onto the downbeat recording starts on, rather than lost.
    static constexpr uint32_t kPreRollTicks = kPpq / 4;

    // Same order as main.cpp's SustainEngine, so it indexes it directly.
    enum Engine : uint8_t
    {
        kGrains,
        kDexedA,
        kDexedB,
        kEnsemble,
        kOrgan,
        kSmpvo,
        kSmpdr,
        kNumEngines
    };
    static const char* EngineName(int engine); // "GR", "DXA", ...

    // Track length as a multiple of the loop: 1/4, 1/2, 1, 2, 4.
    static constexpr int kNumLengths    = 5;
    static constexpr int kDefaultLength = 2; // 1x
    static const char*   LengthName(int length_index);
    // Whether length_index fits kMaxBars for a loop of loop_bars.
    static bool LengthFits(int length_index, int loop_bars);

    // Playback quantise, per track, finest first: Off, 1/32T, 1/32, 1/16T,
    // 1/16, 1/8T, 1/8, 1/4.
    // Applied to a copy that playback reads (the recorded notes never
    // change), moving each note's start to the nearest step and its note
    // off by the same amount so it keeps its length. Swing (0..100%)
    // delays every second step by up to half a step.
    static constexpr int kNumQuant = 8;
    static const char*   QuantName(int quant_index);
    static uint32_t      QuantTicks(int quant_index); // step length, 0 = Off
    // The grid's step for a quantise setting: the same, and 1/16 while
    // quantise is Off. Rolls split a hit's step into their repeats.
    static uint32_t      GridTicks(int quant_index);
    // A track's length in ticks at a loop of loop_bars (what Tick() uses).
    static uint32_t      TrackTicksFor(int length_index, int loop_bars);
    // Roll counts a hit can have (1 = a plain hit).
    static constexpr int kNumRolls = 6;
    static int           RollCount(int roll_index); // 1, 2, 3, 4, 6, 8

    // One recorded event. Read-only to the UI (GetEvents()), for drawing.
    struct Event
    {
        uint32_t tick;
        uint8_t  note;
        uint8_t  velocity; // 0 = note off
        uint8_t  pass;     // which recording pass wrote it (0 = the grid)
        uint8_t  roll;     // note on only: repeats within its step (0/1 = none)
    };

    // The same states, and the same B1 gestures, as a looper layer.
    enum class State : uint8_t
    {
        Empty,
        Playing,
        Paused,
        Armed,     // waiting out the tempo clock's count-in
        Recording, // one pass of the track's length, onto whatever's there
    };

    // Fires a track's note on an engine (vel 0 = note off) -- the track so
    // its arp can play it.
    using NoteFn = void (*)(int track, int engine, uint8_t note, uint8_t velocity);

    // Patterns A..H per track. The one playing lives in events_ (D2); the
    // rest wait in pattern_store, kTracks * kPatterns * kMaxEvents events
    // the caller owns (SDRAM -- 512KB). Each has its own notes, length,
    // quantise and swing; the engine is the track's.
    static constexpr int kPatterns = 8;
    void Init(NoteFn note_fn, Event* pattern_store);

    // --- Control interrupt ------------------------------------------------
    // loop_tick: position in the loop (0..loop_ticks-1); running: the clock
    // is advancing (the looper is on); record_starts: how many times the
    // tempo clock has finished a count-in (TempoTick::record_start) --
    // an Armed track starts recording when this moves on.
    void Tick(uint32_t loop_tick, uint32_t loop_ticks, bool running, uint32_t record_starts);
    // A live note while `track` is selected: recorded if it's recording.
    void OnLiveNote(int track, uint8_t note, uint8_t velocity);

    // --- UI ---------------------------------------------------------------
    // B1 tap, as on a layer: Playing <-> Paused, cancels a count-in, ends
    // a recording early; nothing on an Empty track.
    void RequestTap(int t) { req_tap_[t] = true; }
    // Play or pause a track outright (not a toggle like a tap) -- for
    // printing, which mustn't flip a track that's already where it wants
    // it. Only acts on a Playing/Paused track.
    void RequestPlay(int t, bool play) { req_play_[t] = play ? 1 : 2; }
    // B1 hold, as on a layer: Empty/Playing/Paused -> Armed (the caller
    // also asks the tempo clock for its count-in).
    void RequestRecord(int t) { req_record_[t] = true; }
    static bool CanArm(State s)
    {
        return s == State::Empty || s == State::Playing || s == State::Paused;
    }
    void RequestUndo(int t) { req_undo_[t] = true; }
    // Switch to pattern p when the one playing reaches its end (straight
    // away if the track isn't playing; after a take, if recording).
    void RequestPattern(int t, int p) { tracks_[t].next_pattern = (int8_t)p; }
    // Copy the playing pattern into p (notes and settings) and switch to it
    // as above -- the start of a variation.
    void RequestCopyPattern(int t, int p) { req_copy_[t] = (uint8_t)(p + 1); }
    int  GetPattern(int t) const { return tracks_[t].pattern; }
    int  GetNextPattern(int t) const { return tracks_[t].next_pattern; } // -1 = none
    bool PatternHasNotes(int t, int p) const
    {
        return p == tracks_[t].pattern ? tracks_[t].count > 0 : meta_[t][p].count > 0;
    }
    void RequestClear(int t) { req_clear_[t] = true; }
    // A scene: each track's pattern (-1 = pause it), all switched together
    // where the loop next starts over, every track from its top (so ones
    // longer than the loop line up again); straight away if the clock
    // isn't running. A track that's recording or counting in is left be.
    void RequestScene(const int8_t* patterns, const int8_t (*chains)[8],
                      const uint8_t* chain_lens);
    void CancelScene() { req_scene_ = false; }
    bool ScenePending() const { return req_scene_; }
    // A chain: patterns played one after another, each for its own length,
    // starting over when it's done (A A A B: three of A, then B). Takes
    // over where the loop next starts (straight away if the clock isn't
    // running); fewer than 2 ends it (one is kept, to add to). Choosing a pattern on its own ends
    // it too.
    static constexpr int kMaxChain = 8;
    void RequestChain(int t, const int8_t* chain, int n);
    // The chain (the one asked for, until it takes over); returns its length.
    int  GetChain(int t, int8_t* out) const;
    int  GetChainStep(int t) const { return tracks_[t].chain_len > 1 ? tracks_[t].chain_pos : -1; }
    bool ChainPending(int t) const { return req_chain_[t]; }
    void SetEngine(int t, int e) { tracks_[t].engine = (uint8_t)e; }
    void SetLengthIndex(int t, int i) { tracks_[t].length_index = (uint8_t)i; }
    void SetQuantIndex(int t, int q) { tracks_[t].quant = (uint8_t)q; }

    // Grid edits, queued and applied in Tick(). A note is found by its
    // pitch and its note on lying in [from, to) (a grid cell; `from` can be
    // negative for a cell that straddles the start). len_ticks is the
    // track's length, for wrapping. from = kWholeRow: every note of that
    // pitch in the track (velocity, length and roll only).
    static constexpr int32_t kWholeRow = INT32_MIN;
    void RequestAddNote(int t, uint32_t tick, uint8_t note, uint8_t velocity, uint32_t length,
                        uint32_t len_ticks);
    void RequestDeleteNote(int t, int32_t from, int32_t to, uint8_t note, uint32_t len_ticks);
    void RequestSetVelocity(int t, int32_t from, int32_t to, uint8_t note, uint8_t velocity,
                            uint32_t len_ticks);
    void RequestSetLength(int t, int32_t from, int32_t to, uint8_t note, uint32_t length,
                          uint32_t len_ticks);
    void RequestSetRoll(int t, int32_t from, int32_t to, uint8_t note, int roll_count,
                        uint32_t len_ticks);
    // For drawing (unlocked -- an edit landing mid-frame just draws once
    // slightly stale).
    const Event* GetEvents(int t) const { return events_[t]; }
    // Main loop: what track t plays over len_ticks -- its notes quantised,
    // swung, transposed and rolled as playback does (tick order) -- for
    // working out its arp's notes. Built from the recorded notes, so it's
    // right whether or not the track's playing. Scratch: kMaxEvents events,
    // partner/shift kMaxEvents each; out up to kMaxPlayEvents.
    int BuildPlayCopy(int t, uint32_t len_ticks, Event* out, int max, Event* scratch,
                      int16_t* partner, int32_t* shift) const;
    // Notes (a start, a length, a pitch, a velocity) as a pattern's events:
    // each note on and its note off (round the end if it runs past it), in
    // tick order with offs first at a shared tick -- how a track records.
    // At most `max` events (whole notes only); returns how many.
    struct NoteSpan
    {
        uint32_t tick, length;
        uint8_t  note, velocity;
    };
    static int NotesToEvents(const NoteSpan* notes, int n, uint32_t len_ticks, Event* out, int max);

    // --- Saving (with a Performance, see PerformanceStore::Save()) -------
    // One track's settings as stored. Fixed layout: grows only by
    // appending, inside reserved[] first.
    struct TrackSave
    {
        uint8_t engine, length_index, quant, swing;
        uint8_t paused; // 1 = Paused, else Playing (or Empty if no events)
        uint8_t reserved[3];
        int32_t count;  // events that follow
    };
    // Main loop only. The copies run with interrupts briefly off (a few
    // microseconds), so the 1kHz Tick() never sees a half-written track.
    void SaveTrack(int t, TrackSave* out) const;
    int  CopyEvents(int t, int first, Event* out, int max) const;
    // Before loading: RequestClear() every track and give Tick() a couple
    // of milliseconds to release their notes, then LoadTrack() each one
    // and AppendEvents() its notes.
    void LoadTrack(int t, const TrackSave& s);
    void AppendEvents(int t, const Event* ev, int n);
    // All the patterns: pattern p's settings/events (the playing one's come
    // from the live track), and loading them -- LoadTrack() first (it
    // empties every pattern), then each LoadPattern() + AppendPatternEvents(),
    // then FinishLoad() makes `current` the one playing.
    void SavePattern(int t, int p, TrackSave* out) const;
    int  CopyPatternEvents(int t, int p, int first, Event* out, int max) const;
    void LoadPattern(int t, int p, const TrackSave& s);
    void AppendPatternEvents(int t, int p, const Event* ev, int n);
    void FinishLoad(int t, int current);
    // A loaded chain (after FinishLoad()): it starts with the loop under
    // way, at once -- not after the next loop like RequestChain().
    void LoadChain(int t, const int8_t* chain, int n);
    void SetSwing(int t, int pct) { tracks_[t].swing = (uint8_t)pct; }
    // Pattern p's transpose, -kMaxTranspose..+kMaxTranspose semitones,
    // applied on playback (the notes themselves never move); a take
    // recorded under it is stored shifted back, so it plays as played.
    // Each pattern keeps its own; the change is applied in Tick().
    static constexpr int kMaxTranspose = 24;
    void SetTranspose(int t, int p, int semis)
    {
        req_tp_[t] = (int16_t)(((p + 1) << 8) | (uint8_t)(int8_t)semis);
    }
    int GetTranspose(int t) const { return tracks_[t].transpose; }
    int GetTranspose(int t, int p) const
    {
        return p == tracks_[t].pattern ? tracks_[t].transpose : meta_[t][p].transpose;
    }

    State GetState(int t) const { return tracks_[t].state; }
    int   GetEngine(int t) const { return tracks_[t].engine; }
    int   GetLengthIndex(int t) const { return tracks_[t].length_index; }
    int   GetQuantIndex(int t) const { return tracks_[t].quant; }
    int   GetSwing(int t) const { return tracks_[t].swing; }
    int   GetEventCount(int t) const { return tracks_[t].count; }
    bool  CanUndo(int t) const { return tracks_[t].last_pass != 0; }
    // 0..1 through the track's own length, for drawing a playhead.
    float GetPosition01(int t) const;
    // Where the track is, in ticks (0 = its top).
    uint32_t GetPosTicks(int t) const { return tracks_[t].pos; }

  private:
    struct Track
    {
        volatile State state        = State::Empty;
        volatile uint8_t engine     = kDexedA;
        volatile uint8_t length_index = kDefaultLength;
        volatile uint8_t quant      = 0; // kNumQuant index, 0 = Off
        volatile uint8_t swing      = 0; // percent
        volatile int8_t  transpose  = 0; // semitones, see SetTranspose()
        int8_t   built_transpose = 0;
        // The quantised copy playback reads (see Rebuild()), and what it
        // was built from -- rebuilt whenever any of these change.
        int      play_count    = 0;
        bool     dirty         = true; // the recorded notes changed
        uint8_t  built_quant   = 0xFF;
        uint8_t  built_swing   = 0;
        uint32_t built_ticks   = 0;
        volatile int   count        = 0;
        uint8_t        pass         = 0; // last pass number handed out
        volatile uint8_t last_pass  = 0; // the pass Undo removes (0 = none)
        // Playback.
        int32_t  prev_pos      = -1; // last position played up to (-1 = restart)
        uint32_t ticks         = 0;  // current length in ticks
        volatile uint32_t pos  = 0;  // current position
        uint8_t  sounding_engine = kDexedA;
        bool     sounding[128] = {}; // notes this track has on right now
        bool     rec_held[128] = {}; // live keys down while recording
        State    state_before_arm = State::Empty;
        uint32_t armed_at      = 0; // record_starts when it was armed
        uint32_t rec_elapsed   = 0; // ticks recorded this pass (ends at one length)
        // Notes played while Armed (see kPreRollTicks): when each went down
        // (clock_ ticks, kNoPreRoll = none), its velocity, and how long it
        // was held if it's already up again (0 = still down).
        uint32_t pre_on[128];
        uint32_t pre_len[128];
        uint8_t  pre_vel[128];
        bool     preroll_pending = false; // flush them once pos is the downbeat
        uint8_t  pattern       = 0;   // the one playing (0..kPatterns-1)
        volatile int8_t next_pattern = -1; // queued switch, -1 = none
        bool     from_top      = false; // a scene restarted it: play from 0
        // The chain playing (see RequestChain()): its own clock, since its
        // patterns' lengths needn't line up with the loop.
        int8_t   chain[kMaxChain] = {};
        uint8_t  chain_len     = 0; // < 2 = none
        uint8_t  chain_pos     = 0;
        bool     chain_sync    = false; // (re)start it from this loop's start
        uint32_t step_start    = 0;     // clock_ where this step began
    };
    // The waiting patterns' settings (the playing one's are in Track).
    struct PatternMeta
    {
        uint8_t length_index = kDefaultLength;
        uint8_t quant        = 0;
        uint8_t swing        = 0;
        int8_t  transpose    = 0;
        int32_t count        = 0;
    };
    PatternMeta meta_[kTracks][kPatterns];
    Event*      store_ = nullptr;
    Event*      Store(int t, int p) const { return store_ + ((size_t)t * kPatterns + p) * kMaxEvents; }
    void        SwitchPattern(int t, int p); // Tick() only
    volatile uint8_t req_copy_[kTracks] = {}; // copy-to pattern + 1, 0 = none
    volatile int16_t req_tp_[kTracks]   = {}; // (pattern + 1) << 8 | semitones, 0 = none
    volatile bool    req_scene_         = false;
    int8_t           scene_pat_[kTracks] = {};
    int8_t           scene_chain_[kTracks][kMaxChain] = {};
    uint8_t          scene_chain_len_[kTracks] = {};
    volatile bool    req_chain_[kTracks] = {};
    int8_t           req_chain_pat_[kTracks][kMaxChain] = {};
    uint8_t          req_chain_len_[kTracks] = {};
    void             StartChain(int t, const int8_t* chain, int n); // Tick() only
    void             ApplyScene(); // Tick() only

    uint32_t TrackTicks(const Track& tr, uint32_t loop_ticks) const;
    void     PlayRange(Track& tr, int32_t from_excl, int32_t to_incl);
    void     ReleaseAll(Track& tr);
    void     Insert(int t, uint32_t tick, uint8_t note, uint8_t velocity);
    void     InsertEvent(int t, const Event& ev);
    void     RemoveAt(int t, int i);
    int      FindNoteOn(int t, int32_t from, int32_t to, uint8_t note, uint32_t len_ticks) const;
    int      FindNoteOff(int t, int on_index) const;

    enum class EditType : uint8_t
    {
        Add,
        Delete,
        Velocity,
        Length,
        Roll
    };
    struct Edit
    {
        EditType type;
        uint8_t  track, note, value8;
        int32_t  from, to;
        uint32_t value32, len_ticks;
    };
    static constexpr int kEditQueue = 16;
    Edit              edits_[kEditQueue];
    volatile uint32_t edit_head_ = 0, edit_tail_ = 0; // UI writes head, Tick() tail
    void PushEdit(const Edit& e);
    void ApplyEdit(const Edit& e);
    void     StopRecording(int t);
    void     RemovePass(int t, uint8_t pass);
    void     Rebuild(int t);
    // Rebuild()'s work, on any buffers (BuildPlayCopy() uses it too).
    static int BuildPlay(const Event* src, int n, uint32_t len, uint8_t quant, uint8_t swing,
                         int8_t transpose, Event* dst, int max, int16_t* partner, int32_t* shift);

    Track    tracks_[kTracks];
    Event    events_[kTracks][kMaxEvents];
    Event    play_events_[kTracks][kMaxPlayEvents]; // quantised + rolls, see Rebuild()
    int16_t  partner_[kMaxEvents];              // Rebuild() scratch: each on's note off
    int32_t  shift_[kMaxEvents];                // ... and how far it moved
    volatile bool req_record_[kTracks] = {};
    volatile bool req_tap_[kTracks]    = {};
    volatile uint8_t req_play_[kTracks] = {}; // 0 none, 1 play, 2 pause
    volatile bool req_undo_[kTracks]   = {};
    volatile bool req_clear_[kTracks]  = {};
    NoteFn   note_fn_     = nullptr;
    uint32_t prev_loop_tick_ = 0;
    uint32_t record_starts_  = 0;
    uint32_t clock_          = 0; // ticks the clock has run, never wrapping back
    static constexpr uint32_t kNoPreRoll = 0xFFFFFFFFu;
    void ClearPreRoll(Track& tr);
    void FlushPreRoll(int t);
    uint32_t loop_count_     = 0; // loop wraps seen, for tracks longer than the loop
    bool     was_running_    = false;
};
