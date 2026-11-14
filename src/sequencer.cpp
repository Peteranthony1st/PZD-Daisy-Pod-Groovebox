#include "sequencer.h"
#include <cstring>

namespace
{
// Length as a fraction of the loop, num/den.
constexpr uint8_t kLenNum[MidiSequencer::kNumLengths] = {1, 1, 1, 2, 4};
constexpr uint8_t kLenDen[MidiSequencer::kNumLengths] = {4, 2, 1, 1, 1};
// Quantise step in ticks (kPpq = 96 per beat): Off, 1/32T, 1/32, 1/16T,
// 1/16, 1/8T, 1/8, 1/4.
constexpr uint32_t kQuantTicks[MidiSequencer::kNumQuant] = {0, 8, 12, 16, 24, 32, 48, 96};
} // namespace

const char* MidiSequencer::EngineName(int engine)
{
    static const char* const kNames[kNumEngines] = {"GR", "DXA", "DXB", "ENS", "ORG", "SMV", "SMD", "SSW"};
    return engine >= 0 && engine < kNumEngines ? kNames[engine] : "?";
}

const char* MidiSequencer::LengthName(int length_index)
{
    static const char* const kNames[kNumLengths] = {"1/4", "1/2", "1x", "2x", "4x"};
    return length_index >= 0 && length_index < kNumLengths ? kNames[length_index] : "?";
}

const char* MidiSequencer::QuantName(int quant_index)
{
    static const char* const kNames[kNumQuant]
        = {"Off", "1/32T", "1/32", "1/16T", "1/16", "1/8T", "1/8", "1/4"};
    return quant_index >= 0 && quant_index < kNumQuant ? kNames[quant_index] : "?";
}

uint32_t MidiSequencer::QuantTicks(int quant_index)
{
    return quant_index >= 0 && quant_index < kNumQuant ? kQuantTicks[quant_index] : 0;
}

uint32_t MidiSequencer::GridTicks(int quant_index)
{
    uint32_t g = QuantTicks(quant_index);
    return g == 0 ? kPpq / 4 : g; // 1/16 while quantise is Off
}

uint32_t MidiSequencer::TrackTicksFor(int length_index, int loop_bars)
{
    uint32_t loop_ticks = (uint32_t)loop_bars * 4 * kPpq;
    int      li         = length_index >= 0 && length_index < kNumLengths ? length_index
                                                                           : kDefaultLength;
    uint32_t max_tick   = (uint32_t)kMaxBars * 4 * kPpq;
    while(li > 0 && loop_ticks * kLenNum[li] / kLenDen[li] > max_tick)
        li--;
    return loop_ticks * kLenNum[li] / kLenDen[li];
}

int MidiSequencer::RollCount(int roll_index)
{
    static const int kCounts[kNumRolls] = {1, 2, 3, 4, 6, 8};
    return roll_index >= 0 && roll_index < kNumRolls ? kCounts[roll_index] : 1;
}

void MidiSequencer::PushEdit(const Edit& e)
{
    if(edit_head_ - edit_tail_ >= (uint32_t)kEditQueue)
        return; // full -- the UI can't outrun 1kHz in practice
    edits_[edit_head_ % kEditQueue] = e;
    edit_head_                      = edit_head_ + 1;
}

void MidiSequencer::RequestAddNote(int t, uint32_t tick, uint8_t note, uint8_t velocity,
                                   uint32_t length, uint32_t len_ticks)
{
    PushEdit(Edit{EditType::Add, (uint8_t)t, note, velocity, (int32_t)tick, 0, length, len_ticks});
}

void MidiSequencer::RequestDeleteNote(int t, int32_t from, int32_t to, uint8_t note,
                                      uint32_t len_ticks)
{
    PushEdit(Edit{EditType::Delete, (uint8_t)t, note, 0, from, to, 0, len_ticks});
}

void MidiSequencer::RequestSetVelocity(int t, int32_t from, int32_t to, uint8_t note,
                                       uint8_t velocity, uint32_t len_ticks)
{
    PushEdit(Edit{EditType::Velocity, (uint8_t)t, note, velocity, from, to, 0, len_ticks});
}

void MidiSequencer::RequestSetLength(int t, int32_t from, int32_t to, uint8_t note,
                                     uint32_t length, uint32_t len_ticks)
{
    PushEdit(Edit{EditType::Length, (uint8_t)t, note, 0, from, to, length, len_ticks});
}

void MidiSequencer::RequestSetRoll(int t, int32_t from, int32_t to, uint8_t note, int roll_count,
                                   uint32_t len_ticks)
{
    PushEdit(Edit{EditType::Roll, (uint8_t)t, note, (uint8_t)roll_count, from, to, 0, len_ticks});
}

int MidiSequencer::FindNoteOn(int t, int32_t from, int32_t to, uint8_t note,
                              uint32_t len_ticks) const
{
    const Track& tr = tracks_[t];
    for(int i = 0; i < tr.count; i++)
    {
        const Event& ev = events_[t][i];
        if(ev.velocity == 0 || ev.note != note)
            continue;
        int32_t tk = (int32_t)ev.tick;
        // A cell starting before 0 also catches notes at the very end.
        if((tk >= from && tk < to) || (from < 0 && tk >= from + (int32_t)len_ticks))
            return i;
    }
    return -1;
}

int MidiSequencer::FindNoteOff(int t, int on_index) const
{
    const Track& tr = tracks_[t];
    const uint8_t note = events_[t][on_index].note;
    for(int k = 1; k < tr.count; k++)
    {
        int i = (on_index + k) % tr.count;
        if(events_[t][i].note == note && events_[t][i].velocity == 0)
            return i;
        if(events_[t][i].note == note && events_[t][i].velocity > 0)
            return -1; // re-struck before it was let go
    }
    return -1;
}

void MidiSequencer::RemoveAt(int t, int i)
{
    Track& tr = tracks_[t];
    memmove(&events_[t][i], &events_[t][i + 1], (size_t)(tr.count - 1 - i) * sizeof(Event));
    tr.count = tr.count - 1;
    tr.dirty = true;
}

void MidiSequencer::ApplyEdit(const Edit& e)
{
    const int t = e.track;
    if(t >= kTracks || e.len_ticks == 0)
        return;
    Track& tr = tracks_[t];
    if(e.type == EditType::Add)
    {
        int32_t g = (int32_t)e.value32;
        if(FindNoteOn(t, e.from, e.from + 1, e.note, e.len_ticks) >= 0)
            return; // already a note there
        uint32_t on  = (uint32_t)e.from % e.len_ticks;
        uint32_t off = (on + (g > 0 ? (uint32_t)g : 1)) % e.len_ticks;
        InsertEvent(t, Event{on, e.note, e.value8, 0, 0});
        InsertEvent(t, Event{off, e.note, 0, 0, 0});
        if(tr.state == State::Empty)
            tr.state = State::Playing;
        return;
    }
    if(e.from == kWholeRow)
    {
        // Every note of this pitch: collect their starts first, since a
        // length change moves events around under the loop.
        if(e.type == EditType::Add || e.type == EditType::Delete)
            return;
        int found = 0;
        for(int i = 0; i < tr.count && found < kMaxEvents; i++)
            if(events_[t][i].note == e.note && events_[t][i].velocity > 0)
                shift_[found++] = (int32_t)events_[t][i].tick;
        for(int k = 0; k < found; k++)
        {
            Edit one = e;
            one.from = shift_[k];
            one.to   = shift_[k] + 1;
            ApplyEdit(one);
        }
        return;
    }
    int on = FindNoteOn(t, e.from, e.to, e.note, e.len_ticks);
    if(on < 0)
        return;
    if(tr.sounding[e.note])
    {
        tr.sounding[e.note] = false;
        note_fn_(t, tr.sounding_engine, e.note, 0);
    }
    switch(e.type)
    {
        case EditType::Velocity:
            events_[t][on].velocity = e.value8 > 0 ? e.value8 : 1;
            tr.dirty                = true;
            break;
        case EditType::Roll:
            events_[t][on].roll = e.value8;
            tr.dirty            = true;
            break;
        case EditType::Delete:
        {
            int off = FindNoteOff(t, on);
            if(off >= 0)
            {
                RemoveAt(t, off > on ? off : on);
                RemoveAt(t, off > on ? on : off);
            }
            else
                RemoveAt(t, on);
            if(tr.count == 0 && tr.state != State::Recording && tr.state != State::Armed)
                tr.state = State::Empty;
            break;
        }
        case EditType::Length:
        {
            uint32_t on_tick = events_[t][on].tick;
            int      off     = FindNoteOff(t, on);
            if(off >= 0)
                RemoveAt(t, off);
            uint32_t len = e.value32 > 0 ? e.value32 : 1;
            InsertEvent(t, Event{(on_tick + len) % e.len_ticks, e.note, 0, 0, 0});
            break;
        }
        default: break;
    }
}

void MidiSequencer::Rebuild(int t)
{
    Track& tr          = tracks_[t];
    tr.built_quant     = tr.quant;
    tr.built_swing     = tr.swing;
    tr.built_transpose = tr.transpose;
    tr.built_ticks     = tr.ticks;
    tr.dirty           = false;
    tr.play_count = BuildPlay(events_[t], tr.count, tr.ticks, tr.quant, tr.swing, tr.transpose,
                              play_events_[t], kMaxPlayEvents, partner_, shift_);
}

int MidiSequencer::BuildPlay(const Event* src, int n, uint32_t len, uint8_t quant, uint8_t swing,
                             int8_t transpose, Event* dst, int max, int16_t* partner_, int32_t* shift_)
{
    const uint8_t  q = quant < kNumQuant ? quant : 0;
    const uint32_t g = kQuantTicks[q];
    if(len == 0)
        return 0;

    // Pair each note on with its note off: the next off of the same note
    // after it, going round the end of the track if it's held across it.
    // Two laps; the second only closes notes still open from the first.
    int16_t open[128];
    for(int k = 0; k < 128; k++)
        open[k] = -1;
    for(int i = 0; i < n; i++)
        partner_[i] = -1;
    for(int lap = 0; lap < 2; lap++)
        for(int i = 0; i < n; i++)
        {
            const Event& ev = src[i];
            if(ev.velocity > 0)
            {
                if(lap == 0)
                    open[ev.note] = (int16_t)i;
            }
            else if(open[ev.note] >= 0)
            {
                partner_[open[ev.note]] = (int16_t)i;
                open[ev.note]           = -1;
            }
        }

    // Quantise: snap each on to the nearest step (every second step later
    // by the swing), and move its off by the same amount.
    for(int i = 0; i < n; i++)
        shift_[i] = 0;
    if(g > 0)
    {
        const uint32_t swing_ticks = g * swing / 200;
        for(int i = 0; i < n; i++)
        {
            if(src[i].velocity == 0)
                continue;
            uint32_t step = (src[i].tick + g / 2) / g;
            uint32_t qt   = step * g + ((step & 1) ? swing_ticks : 0);
            int32_t  d    = (int32_t)qt - (int32_t)src[i].tick;
            shift_[i]     = d;
            if(partner_[i] >= 0)
                shift_[partner_[i]] = d;
        }
    }
    auto wrap = [len](int32_t tk) {
        tk %= (int32_t)len;
        return (uint32_t)(tk < 0 ? tk + (int32_t)len : tk);
    };
    for(int i = 0; i < n; i++)
    {
        dst[i]      = src[i];
        dst[i].tick = wrap((int32_t)src[i].tick + shift_[i]);
        int note    = (int)src[i].note + transpose; // a note pushed past either end stays there
        dst[i].note = (uint8_t)(note < 0 ? 0 : (note > 127 ? 127 : note));
    }

    // Rolls: a hit with a roll plays that many evenly spaced repeats
    // across its step (or across the note, if it's longer), each held
    // half its slot.
    int       out  = n;
    const int cell = (int)GridTicks(q);
    for(int i = 0; i < n; i++)
    {
        int r = dst[i].roll;
        if(dst[i].velocity == 0 || r < 2)
            continue;
        int32_t on_tk   = (int32_t)dst[i].tick;
        int32_t note_ln = partner_[i] >= 0 ? (int32_t)wrap((int32_t)dst[partner_[i]].tick - on_tk)
                                           : cell;
        int32_t window  = note_ln > cell ? note_ln : cell;
        int32_t sub     = window / r > 0 ? window / r : 1;
        int32_t hold    = sub / 2 > 0 ? sub / 2 : 1;
        if(partner_[i] >= 0)
            dst[partner_[i]].tick = wrap(on_tk + hold);
        for(int k = 1; k < r && out + 2 <= max; k++)
        {
            dst[out]        = dst[i];
            dst[out].tick   = wrap(on_tk + k * sub);
            dst[out + 1]    = dst[i];
            dst[out + 1].velocity = 0;
            dst[out + 1].tick     = wrap(on_tk + k * sub + hold);
            out += 2;
        }
    }

    // Back into tick order (offs first at a shared tick, so a note
    // re-struck on the same step retriggers). Mostly in order already, so
    // a stable insertion sort is quick.
    for(int i = 1; i < out; i++)
    {
        Event e = dst[i];
        int   j = i;
        while(j > 0
              && (dst[j - 1].tick > e.tick
                  || (dst[j - 1].tick == e.tick && dst[j - 1].velocity > 0 && e.velocity == 0)))
        {
            dst[j] = dst[j - 1];
            j--;
        }
        dst[j] = e;
    }
    return out;
}

bool MidiSequencer::LengthFits(int length_index, int loop_bars)
{
    return loop_bars * kLenNum[length_index] / kLenDen[length_index] <= kMaxBars;
}

void MidiSequencer::Init(NoteFn note_fn, Event* pattern_store)
{
    note_fn_ = note_fn;
    store_   = pattern_store;
    for(Track& tr : tracks_)
        ClearPreRoll(tr);
}

void MidiSequencer::SwitchPattern(int t, int p)
{
    Track& tr = tracks_[t];
    if(p == tr.pattern || p < 0 || p >= kPatterns || !store_)
        return;
    ReleaseAll(tr);
    // Park the playing pattern, bring the new one in.
    const int   cur = tr.pattern;
    PatternMeta& m  = meta_[t][cur];
    memcpy(Store(t, cur), events_[t], (size_t)tr.count * sizeof(Event));
    m.length_index = tr.length_index;
    m.quant        = tr.quant;
    m.swing        = tr.swing;
    m.transpose    = tr.transpose;
    m.count        = tr.count;
    const PatternMeta& n = meta_[t][p];
    memcpy(events_[t], Store(t, p), (size_t)n.count * sizeof(Event));
    tr.length_index = n.length_index;
    tr.quant        = n.quant;
    tr.swing        = n.swing;
    tr.transpose    = n.transpose;
    tr.count        = n.count;
    tr.pattern      = (uint8_t)p;
    tr.last_pass    = 0; // the new pattern's takes can't be undone from here
    tr.dirty        = true;
    // An empty pattern leaves the track Empty; one with notes plays (or
    // stays paused).
    if(tr.count == 0 && tr.state == State::Playing)
        tr.state = State::Empty;
    else if(tr.count > 0 && tr.state == State::Empty)
        tr.state = State::Playing;
}

void MidiSequencer::RequestScene(const int8_t* patterns, const int8_t (*chains)[8],
                                 const uint8_t* chain_lens)
{
    req_scene_ = false; // Tick() mustn't take a half-copied scene
    for(int t = 0; t < kTracks; t++)
    {
        scene_pat_[t]       = patterns[t];
        scene_chain_len_[t] = chain_lens[t] <= kMaxChain ? chain_lens[t] : 0;
        memcpy(scene_chain_[t], chains[t], kMaxChain);
    }
    req_scene_ = true;
}

void MidiSequencer::RequestChain(int t, const int8_t* chain, int n)
{
    req_chain_[t] = false;
    n             = n < 0 ? 0 : n > kMaxChain ? kMaxChain : n;
    if(n > 0)
        memcpy(req_chain_pat_[t], chain, (size_t)n);
    req_chain_len_[t] = (uint8_t)n;
    req_chain_[t]     = true;
}

int MidiSequencer::GetChain(int t, int8_t* out) const
{
    const bool    req = req_chain_[t];
    const int     n   = req ? req_chain_len_[t] : tracks_[t].chain_len;
    const int8_t* src = req ? req_chain_pat_[t] : tracks_[t].chain;
    memcpy(out, src, (size_t)n);
    return n;
}

void MidiSequencer::StartChain(int t, const int8_t* chain, int n)
{
    Track& tr = tracks_[t];
    for(int i = 0; i < n; i++)
        if(chain[i] < 0 || chain[i] >= kPatterns)
            n = 0; // never play a bad one
    memcpy(tr.chain, chain, (size_t)n);
    tr.chain_len = (uint8_t)n;
    tr.chain_pos = 0;
    if(n < 2)
        return; // one step is kept for adding to, but doesn't play as a chain
    tr.next_pattern = -1;
    tr.chain_sync   = true;
    SwitchPattern(t, chain[0]);
    tr.from_top = true;
}

void MidiSequencer::ApplyScene()
{
    req_scene_  = false;
    loop_count_ = 0; // tracks longer than the loop start from their top
    for(int t = 0; t < kTracks; t++)
    {
        Track& tr = tracks_[t];
        if(tr.state == State::Recording || tr.state == State::Armed)
            continue;
        const int p     = scene_pat_[t];
        tr.next_pattern = -1;
        ReleaseAll(tr);
        if(p < 0 || p >= kPatterns)
        {
            tr.chain_len = 0;
            if(tr.state == State::Playing)
                tr.state = State::Paused;
            continue;
        }
        if(scene_chain_len_[t] >= 2)
            StartChain(t, scene_chain_[t], scene_chain_len_[t]);
        else
        {
            tr.chain_len = 0;
            SwitchPattern(t, p);
        }
        if(tr.state == State::Paused && tr.count > 0)
            tr.state = State::Playing;
        tr.from_top = true;
    }
}

void MidiSequencer::ClearPreRoll(Track& tr)
{
    for(int n = 0; n < 128; n++)
        tr.pre_on[n] = kNoPreRoll;
    tr.preroll_pending = false;
}

void MidiSequencer::FlushPreRoll(int t)
{
    // Recording has just started on a downbeat (tr.pos): notes played just
    // before it join the take there -- a key still down carries on into
    // it, one already let go keeps its length.
    Track& tr = tracks_[t];
    for(int n = 0; n < 128; n++)
    {
        if(tr.pre_on[n] == kNoPreRoll)
            continue;
        if(clock_ - tr.pre_on[n] <= kPreRollTicks) // pressed close enough to the downbeat
        {
            Insert(t, tr.pos, (uint8_t)n, tr.pre_vel[n]);
            if(tr.pre_len[n] == 0)
                tr.rec_held[n] = true; // its NoteOff is recorded when it comes
            else
                Insert(t, (tr.pos + tr.pre_len[n]) % tr.ticks, (uint8_t)n, 0);
        }
        tr.pre_on[n] = kNoPreRoll;
    }
    tr.preroll_pending = false;
}

float MidiSequencer::GetPosition01(int t) const
{
    const Track& tr = tracks_[t];
    return tr.ticks > 0 ? (float)tr.pos / (float)tr.ticks : 0.f;
}

uint32_t MidiSequencer::TrackTicks(const Track& tr, uint32_t loop_ticks) const
{
    // Longest length that still fits kMaxBars, so a setting chosen at a
    // shorter loop can't run past it after Bars goes up.
    int      li       = tr.length_index < kNumLengths ? tr.length_index : kDefaultLength;
    uint32_t max_tick = (uint32_t)kMaxBars * 4 * kPpq;
    while(li > 0 && loop_ticks * kLenNum[li] / kLenDen[li] > max_tick)
        li--;
    return loop_ticks * kLenNum[li] / kLenDen[li];
}

void MidiSequencer::ReleaseAll(Track& tr)
{
    for(int n = 0; n < 128; n++)
        if(tr.sounding[n])
        {
            tr.sounding[n] = false;
            if(note_fn_)
                note_fn_((int)(&tr - tracks_), tr.sounding_engine, (uint8_t)n, 0);
        }
}

void MidiSequencer::PlayRange(Track& tr, int32_t from_excl, int32_t to_incl)
{
    const int t = (int)(&tr - tracks_);
    // Events are sorted by tick: binary-search the first one after
    // from_excl, so a full track costs no more per step than an empty one
    // (this runs every millisecond for all 8 tracks).
    const Event* evs = play_events_[t];
    int          lo = 0, hi = tr.play_count;
    while(lo < hi)
    {
        int mid = (lo + hi) / 2;
        if((int32_t)evs[mid].tick <= from_excl)
            lo = mid + 1;
        else
            hi = mid;
    }
    for(int i = lo; i < tr.play_count; i++)
    {
        const Event& ev = evs[i];
        if((int32_t)ev.tick > to_incl)
            break;
        // This pass's own notes were played live as they were recorded;
        // they join playback once the pass is over (quantise can move one
        // a little later, which would otherwise sound it twice).
        if(tr.state == State::Recording && ev.pass == tr.pass)
            continue;
        if(ev.velocity > 0)
        {
            tr.sounding[ev.note] = true;
            note_fn_((int)(&tr - tracks_), tr.sounding_engine, ev.note, ev.velocity);
        }
        else if(tr.sounding[ev.note])
        {
            tr.sounding[ev.note] = false;
            note_fn_((int)(&tr - tracks_), tr.sounding_engine, ev.note, 0);
        }
    }
}

void MidiSequencer::Insert(int t, uint32_t tick, uint8_t note, uint8_t velocity)
{
    InsertEvent(t, Event{tick, note, velocity, tracks_[t].pass, 0});
}

void MidiSequencer::InsertEvent(int t, const Event& ev)
{
    Track& tr = tracks_[t];
    if(tr.count >= kMaxEvents)
        return; // full -- drop it rather than overwrite anything
    // After any event at the same tick, so a note recorded on top of a
    // playing one keeps the order it happened in.
    int i = tr.count;
    while(i > 0 && events_[t][i - 1].tick > ev.tick)
        i--;
    memmove(&events_[t][i + 1], &events_[t][i], (size_t)(tr.count - i) * sizeof(Event));
    events_[t][i] = ev;
    tr.count      = tr.count + 1;
    tr.dirty      = true;
}

void MidiSequencer::RemovePass(int t, uint8_t pass)
{
    Track& tr = tracks_[t];
    int    w  = 0;
    for(int r = 0; r < tr.count; r++)
        if(events_[t][r].pass != pass)
            events_[t][w++] = events_[t][r];
    tr.count = w;
    tr.dirty = true;
}

void MidiSequencer::StopRecording(int t)
{
    Track& tr = tracks_[t];
    // A key still down when recording stops gets its note off here, so
    // nothing recorded can hang.
    for(int n = 0; n < 128; n++)
        if(tr.rec_held[n])
        {
            tr.rec_held[n] = false;
            Insert(t, tr.pos, (uint8_t)n, 0);
        }
    tr.state = tr.count > 0 ? State::Playing : State::Empty;
    if(tr.count == 0)
        tr.last_pass = 0;
}

void MidiSequencer::OnLiveNote(int t, uint8_t note, uint8_t velocity)
{
    if(t < 0 || t >= kTracks || note > 127)
        return;
    Track& tr = tracks_[t];
    // Stored as it would be before the pattern's transpose, so it plays
    // back as played.
    {
        int n = (int)note - tr.transpose;
        note  = (uint8_t)(n < 0 ? 0 : (n > 127 ? 127 : n));
    }
    if(tr.state == State::Armed)
    {
        // Remembered in case it's just ahead of the downbeat (see
        // FlushPreRoll()).
        if(velocity > 0)
        {
            tr.pre_on[note]  = clock_;
            tr.pre_vel[note] = velocity;
            tr.pre_len[note] = 0;
        }
        else if(tr.pre_on[note] != kNoPreRoll && tr.pre_len[note] == 0)
            tr.pre_len[note] = clock_ - tr.pre_on[note] > 0 ? clock_ - tr.pre_on[note] : 1;
        return;
    }
    if(tr.state != State::Recording)
        return;
    if(velocity > 0)
        tr.rec_held[note] = true;
    else if(!tr.rec_held[note])
        return; // a key that went down before recording started
    else
        tr.rec_held[note] = false;
    // At the position already played up to, so playback doesn't fire the
    // note a second time on its next step.
    Insert(t, tr.pos, note, velocity);
}

void MidiSequencer::Tick(uint32_t loop_tick, uint32_t loop_ticks, bool running,
                         uint32_t record_starts)
{
    if(!note_fn_)
        return;
    record_starts_ = record_starts;

    // Grid edits from the UI.
    while(edit_tail_ != edit_head_)
    {
        ApplyEdit(edits_[edit_tail_ % kEditQueue]);
        edit_tail_ = edit_tail_ + 1;
    }

    // Requests from the UI first.
    for(int t = 0; t < kTracks; t++)
    {
        Track& tr = tracks_[t];
        if(req_clear_[t])
        {
            req_clear_[t] = false;
            ReleaseAll(tr);
            memset(tr.rec_held, 0, sizeof(tr.rec_held));
            tr.count     = 0;
            tr.last_pass = 0;
            tr.state     = State::Empty;
            tr.dirty     = true;
            tr.chain_len = 0;
        }
        if(req_undo_[t])
        {
            req_undo_[t] = false;
            if(tr.last_pass != 0)
            {
                ReleaseAll(tr);
                if(tr.state == State::Recording)
                    memset(tr.rec_held, 0, sizeof(tr.rec_held));
                RemovePass(t, tr.last_pass);
                tr.last_pass = 0;
                if(tr.state != State::Recording)
                    tr.state = tr.count > 0 ? State::Playing : State::Empty;
            }
        }
        if(req_tap_[t])
        {
            req_tap_[t] = false;
            switch(tr.state)
            {
                case State::Empty: break;
                case State::Playing:
                    ReleaseAll(tr);
                    tr.state = State::Paused;
                    break;
                case State::Paused: tr.state = State::Playing; break;
                case State::Armed: // cancel the count-in
                    tr.state = tr.state_before_arm;
                    ClearPreRoll(tr);
                    break;
                case State::Recording: StopRecording(t); break;          // end it early
            }
        }
        if(req_copy_[t] != 0)
        {
            const int dst = req_copy_[t] - 1;
            req_copy_[t]  = 0;
            if(dst >= 0 && dst < kPatterns && dst != tr.pattern && store_)
            {
                memcpy(Store(t, dst), events_[t], (size_t)tr.count * sizeof(Event));
                PatternMeta& m = meta_[t][dst];
                m.length_index = tr.length_index;
                m.quant        = tr.quant;
                m.swing        = tr.swing;
                m.transpose    = tr.transpose;
                m.count        = tr.count;
                tr.next_pattern = (int8_t)dst;
            }
        }
        if(req_tp_[t] != 0)
        {
            const int16_t r = req_tp_[t];
            req_tp_[t]      = 0;
            const int p     = (r >> 8) - 1;
            const int8_t tp = (int8_t)(r & 0xff);
            if(p == tr.pattern)
                tr.transpose = tp;
            else if(p >= 0 && p < kPatterns)
                meta_[t][p].transpose = tp;
        }
        if(tr.next_pattern >= 0 && tr.chain_len >= 2)
            tr.chain_len = 0; // a pattern chosen on its own ends the chain
        // A chain asked for: now if the clock's stopped, else at the loop's start.
        if(req_chain_[t] && (!running || loop_ticks == 0))
        {
            req_chain_[t] = false;
            StartChain(t, req_chain_pat_[t], req_chain_len_[t]);
        }
        // A track that isn't playing switches pattern straight away.
        if(tr.next_pattern >= 0 && tr.state != State::Playing && tr.state != State::Recording
           && tr.state != State::Armed)
        {
            SwitchPattern(t, tr.next_pattern);
            tr.next_pattern = -1;
        }
        if(req_play_[t] != 0)
        {
            const bool play = req_play_[t] == 1;
            req_play_[t]    = 0;
            if(play && tr.state == State::Paused)
                tr.state = State::Playing;
            else if(!play && tr.state == State::Playing)
            {
                ReleaseAll(tr);
                tr.state = State::Paused;
            }
        }
        if(req_record_[t])
        {
            req_record_[t] = false;
            if(CanArm(tr.state))
            {
                ClearPreRoll(tr);
                tr.state_before_arm = tr.state;
                tr.armed_at         = record_starts_;
                tr.state            = State::Armed;
            }
        }
        // The count-in finished: recording starts on this downbeat.
        if(tr.state == State::Armed && record_starts_ != tr.armed_at)
        {
            tr.pass        = (uint8_t)(tr.pass == 255 ? 1 : tr.pass + 1);
            tr.last_pass   = tr.pass;
            tr.rec_elapsed     = 0;
            tr.state           = State::Recording;
            tr.preroll_pending = true;
        }
        // A new engine: let go of what the old one holds.
        if(tr.engine != tr.sounding_engine)
            ReleaseAll(tr);
        tr.sounding_engine = tr.engine;
    }

    if(!running || loop_ticks == 0)
    {
        if(req_scene_)
            ApplyScene();
        if(was_running_)
            for(int t = 0; t < kTracks; t++)
            {
                ReleaseAll(tracks_[t]);
                tracks_[t].prev_pos = -1;
            }
        was_running_    = false;
        prev_loop_tick_ = loop_tick;
        return;
    }

    // Loop wraps (for tracks longer than the loop) and backward jumps
    // (scrubbing): a small step back is a jump, not a wrap.
    bool jumped = false, wrapped = false;
    if(!was_running_)
    {
        // The looper's (re)started from bar 1: so does everything -- a
        // chain too, which keeps its own time and would otherwise carry on
        // from wherever it was when the looper stopped.
        loop_count_ = 0;
        for(int t = 0; t < kTracks; t++)
        {
            Track& tr = tracks_[t];
            if(tr.chain_len < 2 || tr.state == State::Recording || tr.state == State::Armed)
                continue;
            tr.chain_pos  = 0;
            tr.chain_sync = true;
            if(tr.chain[0] != tr.pattern)
                SwitchPattern(t, tr.chain[0]);
            tr.from_top = true;
        }
    }
    else if(loop_tick < prev_loop_tick_)
    {
        if(prev_loop_tick_ - loop_tick > loop_ticks / 2)
        {
            loop_count_++;
            wrapped = true;
        }
        else
            jumped = true;
    }
    if(wrapped && req_scene_)
        ApplyScene();
    if(wrapped)
        for(int t = 0; t < kTracks; t++)
            if(req_chain_[t])
            {
                req_chain_[t] = false;
                StartChain(t, req_chain_pat_[t], req_chain_len_[t]);
            }
    if(was_running_ && !jumped)
        clock_ += loop_tick >= prev_loop_tick_ ? loop_tick - prev_loop_tick_
                                               : loop_tick + loop_ticks - prev_loop_tick_;
    was_running_    = true;
    prev_loop_tick_ = loop_tick;

    for(int t = 0; t < kTracks; t++)
    {
        Track&   tr  = tracks_[t];
        uint32_t len = TrackTicks(tr, loop_ticks);
        if(len == 0)
            continue;
        uint32_t pos;
        if(tr.chain_len >= 2)
        {
            // A chain keeps its own time: each step runs its pattern's
            // length from where the last one ended.
            if(tr.chain_sync)
            {
                tr.chain_sync = false;
                tr.chain_pos  = 0;
                tr.step_start = clock_ - loop_tick;
            }
            uint32_t el = clock_ - tr.step_start;
            for(int guard = 0; el >= len && guard < 64; guard++)
            {
                tr.step_start += len;
                el -= len;
                if(tr.state == State::Recording || tr.state == State::Armed)
                    continue; // a take stays on its pattern
                tr.chain_pos = (uint8_t)((tr.chain_pos + 1) % tr.chain_len);
                const int p  = tr.chain[tr.chain_pos];
                if(p != tr.pattern)
                {
                    SwitchPattern(t, p);
                    len         = TrackTicks(tr, loop_ticks);
                    tr.ticks    = len;
                    tr.from_top = true;
                }
            }
            pos = el < len ? el : 0;
        }
        else if(len <= loop_ticks)
            pos = loop_tick % len;
        else
        {
            uint32_t loops = len / loop_ticks;
            pos            = (loop_count_ % loops) * loop_ticks + loop_tick;
        }
        if((len != tr.ticks && tr.prev_pos >= 0) || jumped)
        {
            // Length changed (Bars/BPM, or the setting) or a scrub: start
            // fresh from here rather than replaying a stretch. Not a track
            // starting from its top (prev_pos -1: the clock's just started,
            // or it's just loaded) -- that plays from 0, notes there and all.
            ReleaseAll(tr);
            tr.prev_pos = (int32_t)pos;
        }
        // How far this step moved, for ending a recording after one pass.
        uint32_t advance = 0;
        if(tr.prev_pos >= 0 && len == tr.ticks && !jumped)
            advance = pos >= (uint32_t)tr.prev_pos ? pos - (uint32_t)tr.prev_pos
                                                    : len - (uint32_t)tr.prev_pos + pos;
        tr.ticks = len;
        tr.pos   = pos;

        if(tr.preroll_pending)
            FlushPreRoll(t);
        // A new quantise/swing/length moves notes: let go of what's sounding
        // first, so no note off is left behind where playback already went.
        if(tr.quant != tr.built_quant || tr.swing != tr.built_swing || len != tr.built_ticks
           || tr.transpose != tr.built_transpose)
        {
            ReleaseAll(tr);
            Rebuild(t);
        }
        else if(tr.dirty)
            Rebuild(t);
        if(tr.state == State::Recording)
        {
            tr.rec_elapsed += advance;
            if(tr.rec_elapsed >= len)
                StopRecording(t); // one full pass, like a layer's recording
        }

        // Paused or Armed-from-Paused stays silent; a count-in from
        // Playing keeps playing so you hear what you'll record over.
        const bool silent = tr.state == State::Paused || tr.state == State::Empty
                            || (tr.state == State::Armed && tr.state_before_arm != State::Playing);
        if(silent || tr.count == 0)
        {
            tr.from_top = false;
            tr.prev_pos = (int32_t)pos;
            continue;
        }
        if(tr.from_top)
        {
            // A scene started it over: play what's due from 0.
            tr.from_top = false;
            PlayRange(tr, -1, (int32_t)pos);
            tr.prev_pos = (int32_t)pos;
            continue;
        }
        int32_t prev = tr.prev_pos;
        if(tr.next_pattern == (int8_t)tr.pattern) // chose the one already playing
            tr.next_pattern = -1;
        // A queued pattern takes over where this one wraps (not mid-take).
        if(tr.next_pattern >= 0 && prev >= 0 && (int32_t)pos < prev
           && tr.state == State::Playing)
        {
            const int p     = tr.next_pattern;
            tr.next_pattern = -1;
            SwitchPattern(t, p);
            // The new one's length can differ: pick up from the start of
            // the loop next step, playing what's due from 0.
            tr.prev_pos = -1;
            if(TrackTicks(tr, loop_ticks) != len)
                continue;
            Rebuild(t);
            PlayRange(tr, -1, (int32_t)pos);
            tr.prev_pos = (int32_t)pos;
            continue;
        }
        if(prev < 0)
            prev = (int32_t)pos - 1; // (re)started: play what's on this tick
        if((int32_t)pos >= prev)
            PlayRange(tr, prev, (int32_t)pos);
        else
        {
            // Wrapped past the track's end.
            PlayRange(tr, prev, (int32_t)len - 1);
            PlayRange(tr, -1, (int32_t)pos);
        }
        tr.prev_pos = (int32_t)pos;
    }
}

namespace
{
// Interrupts off around a copy the 1kHz Tick() also touches, restoring
// whatever state they were in.
struct IrqGuard
{
    uint32_t primask = __get_PRIMASK();
    IrqGuard() { __disable_irq(); }
    ~IrqGuard() { __set_PRIMASK(primask); }
};
} // namespace

void MidiSequencer::SaveTrack(int t, TrackSave* out) const
{
    const Track& tr   = tracks_[t];
    *out              = TrackSave{};
    out->engine       = tr.engine;
    out->length_index = tr.length_index;
    out->quant        = tr.quant;
    out->swing        = tr.swing;
    // A count-in or a take in progress saves as what it would become.
    out->paused = tr.state == State::Paused
                  || (tr.state == State::Armed && tr.state_before_arm == State::Paused);
    out->count  = tr.count;
}

int MidiSequencer::CopyEvents(int t, int first, Event* out, int max) const
{
    IrqGuard  g;
    const int n = tracks_[t].count - first;
    const int k = n < max ? (n > 0 ? n : 0) : max;
    memcpy(out, &events_[t][first], (size_t)k * sizeof(Event));
    return k;
}

void MidiSequencer::LoadTrack(int t, const TrackSave& s)
{
    IrqGuard g;
    Track&   tr     = tracks_[t];
    tr.engine       = s.engine < kNumEngines ? s.engine : kDexedA;
    tr.length_index = s.length_index < kNumLengths ? s.length_index : kDefaultLength;
    tr.quant        = s.quant < kNumQuant ? s.quant : 0;
    tr.swing        = s.swing <= 100 ? s.swing : 0;
    tr.count        = 0;
    tr.pass         = 0;
    tr.last_pass    = 0; // a loaded track has no pass to undo
    tr.prev_pos     = -1;
    tr.dirty        = true;
    tr.state        = s.paused ? State::Paused : State::Playing; // Empty below if no events
    tr.pattern      = 0;
    tr.next_pattern = -1;
    tr.chain_len    = 0;
    tr.transpose    = 0;
    memset(tr.rec_held, 0, sizeof(tr.rec_held));
    for(int p = 0; p < kPatterns; p++)
        meta_[t][p] = PatternMeta{};
}

void MidiSequencer::AppendEvents(int t, const Event* ev, int n)
{
    IrqGuard g;
    Track&   tr = tracks_[t];
    if(n > kMaxEvents - tr.count)
        n = kMaxEvents - tr.count;
    if(n > 0)
    {
        memcpy(&events_[t][tr.count], ev, (size_t)n * sizeof(Event));
        tr.count = tr.count + n;
        tr.dirty = true;
    }
    if(tr.count == 0)
        tr.state = State::Empty;
}

void MidiSequencer::SavePattern(int t, int p, TrackSave* out) const
{
    const Track& tr = tracks_[t];
    *out            = TrackSave{};
    out->engine     = tr.engine;
    // reserved[0] holds the transpose (a signed byte).
    if(p == tr.pattern)
    {
        out->length_index = tr.length_index;
        out->quant        = tr.quant;
        out->swing        = tr.swing;
        out->reserved[0]  = (uint8_t)tr.transpose;
        out->count        = tr.count;
    }
    else
    {
        const PatternMeta& m = meta_[t][p];
        out->length_index    = m.length_index;
        out->quant           = m.quant;
        out->swing           = m.swing;
        out->reserved[0]     = (uint8_t)m.transpose;
        out->count           = m.count;
    }
}

int MidiSequencer::CopyPatternEvents(int t, int p, int first, Event* out, int max) const
{
    if(p == tracks_[t].pattern)
        return CopyEvents(t, first, out, max);
    IrqGuard  g;
    const int n = meta_[t][p].count - first;
    const int k = n < max ? (n > 0 ? n : 0) : max;
    memcpy(out, Store(t, p) + first, (size_t)k * sizeof(Event));
    return k;
}

void MidiSequencer::LoadChain(int t, const int8_t* chain, int n)
{
    IrqGuard g;
    req_chain_[t] = false;
    StartChain(t, chain, n < 0 ? 0 : n > kMaxChain ? kMaxChain : n);
}

void MidiSequencer::LoadPattern(int t, int p, const TrackSave& s)
{
    IrqGuard     g;
    PatternMeta& m = meta_[t][p];
    m.length_index = s.length_index < kNumLengths ? s.length_index : kDefaultLength;
    m.quant        = s.quant < kNumQuant ? s.quant : 0;
    m.swing        = s.swing <= 100 ? s.swing : 0;
    int8_t tp      = (int8_t)s.reserved[0];
    m.transpose    = tp < -kMaxTranspose || tp > kMaxTranspose ? 0 : tp;
    m.count        = 0;
}

void MidiSequencer::AppendPatternEvents(int t, int p, const Event* ev, int n)
{
    IrqGuard     g;
    PatternMeta& m = meta_[t][p];
    if(n > kMaxEvents - m.count)
        n = kMaxEvents - m.count;
    if(n > 0 && store_)
    {
        memcpy(Store(t, p) + m.count, ev, (size_t)n * sizeof(Event));
        m.count += n;
    }
}

void MidiSequencer::FinishLoad(int t, int current)
{
    IrqGuard g;
    Track&   tr = tracks_[t];
    if(current < 0 || current >= kPatterns || !store_)
        current = 0;
    const PatternMeta& m = meta_[t][current];
    memcpy(events_[t], Store(t, current), (size_t)m.count * sizeof(Event));
    tr.length_index = m.length_index;
    tr.quant        = m.quant;
    tr.swing        = m.swing;
    tr.transpose    = m.transpose;
    tr.count        = m.count;
    tr.pattern      = (uint8_t)current;
    tr.next_pattern = -1;
    tr.dirty        = true;
    if(tr.count == 0)
        tr.state = State::Empty;
}

int MidiSequencer::BuildPlayCopy(int t, uint32_t len_ticks, Event* out, int max, Event* scratch,
                                 int16_t* partner, int32_t* shift) const
{
    // The recorded notes and their settings, copied together (a brief
    // pause of the interrupt -- a memcpy, not the build), then built here
    // in the main loop exactly as Rebuild() builds the playback copy.
    int     n;
    uint8_t q, sw;
    int8_t  tp;
    {
        IrqGuard     g;
        const Track& tr = tracks_[t];
        n               = tr.count;
        memcpy(scratch, events_[t], (size_t)n * sizeof(Event));
        q  = tr.quant;
        sw = tr.swing;
        tp = tr.transpose;
    }
    return BuildPlay(scratch, n, len_ticks, q, sw, tp, out, max, partner, shift);
}

int MidiSequencer::NotesToEvents(const NoteSpan* notes, int n, uint32_t len_ticks, Event* out, int max)
{
    if(len_ticks == 0)
        return 0;
    int count = 0;
    for(int i = 0; i < n && count + 2 <= max; i++)
    {
        uint32_t ln = notes[i].length < 1 ? 1 : notes[i].length;
        if(ln >= len_ticks)
            ln = len_ticks - 1; // a note can't outlast the pattern
        Event on{};
        on.tick     = notes[i].tick % len_ticks;
        on.note     = notes[i].note;
        on.velocity = notes[i].velocity < 1 ? 1 : notes[i].velocity;
        Event off   = on;
        off.velocity = 0;
        off.tick     = (on.tick + ln) % len_ticks;
        out[count++] = on;
        out[count++] = off;
    }
    // Tick order, offs before ons at a shared tick (so a note struck again
    // on the step it ends retriggers), stable.
    for(int i = 1; i < count; i++)
    {
        Event e = out[i];
        int   j = i;
        while(j > 0
              && (out[j - 1].tick > e.tick
                  || (out[j - 1].tick == e.tick && out[j - 1].velocity > 0 && e.velocity == 0)))
        {
            out[j] = out[j - 1];
            j--;
        }
        out[j] = e;
    }
    return count;
}

