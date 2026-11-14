#include "arpeggiator.h"
#include <cstdio>
#include <initializer_list>

namespace
{
#define REST {Arpeggiator::kRest, 0, 0, 0, 0, 1}
#define TIE {Arpeggiator::kTie, 0, 0, 0, 0, 1}
// The factory phrases -- generated from the verified phrase list (see
// DESIGN.md, Arpeggiator: Phrase mode). A step: {scale step, velocity
// 0 ghost..3 accent, length 0 short/1 normal/2 long, octave, chance,
// ratchet}, or REST/TIE.
const Arpeggiator::Phrase kPhrases[Arpeggiator::kNumPhrases] = {
    {"Acid", 16, {{0, 3, 1, 0, 0, 1}, {0, 0, 0, 0, 0, 1}, {7, 2, 0, 0, 0, 1}, {0, 2, 1, 0, 0, 1}, {2, 2, 2, 0, 0, 1}, TIE, {7, 3, 1, 0, 0, 1}, {0, 0, 0, 0, 0, 1}, {0, 2, 0, 0, 0, 1}, {6, 2, 1, 0, 0, 1}, {7, 3, 1, 0, 0, 2}, {0, 0, 0, 0, 0, 1}, {4, 2, 2, 0, 0, 1}, TIE, {7, 2, 1, 0, 0, 1}, {2, 2, 1, 0, 1, 1}}},
    {"Funk", 16, {{0, 3, 1, -1, 0, 1}, REST, {0, 0, 0, 0, 0, 1}, {7, 2, 0, -1, 0, 1}, REST, {0, 0, 0, -1, 0, 1}, {4, 3, 1, -1, 0, 1}, REST, {0, 2, 1, -1, 0, 1}, {0, 0, 0, 0, 0, 1}, REST, {6, 2, 0, -1, 0, 1}, {7, 3, 1, -1, 0, 1}, REST, {4, 0, 0, -1, 0, 1}, {5, 2, 1, -1, 1, 1}}},
    {"Octaves", 8, {{0, 3, 1, -1, 0, 1}, {7, 2, 0, -1, 0, 1}, {0, 1, 1, -1, 0, 1}, {7, 2, 0, -1, 0, 1}, {0, 3, 1, -1, 0, 1}, {7, 2, 0, -1, 0, 1}, {0, 1, 1, -1, 0, 1}, {7, 2, 0, -1, 0, 2}}},
    {"Walk", 8, {{0, 3, 1, -1, 0, 1}, {2, 2, 1, -1, 0, 1}, {4, 2, 1, -1, 0, 1}, {5, 2, 1, -1, 0, 1}, {7, 3, 1, -1, 0, 1}, {5, 2, 1, -1, 0, 1}, {4, 2, 1, -1, 0, 1}, {1, 2, 1, -1, 0, 1}}},
    {"Disco", 8, {{0, 3, 0, -1, 0, 1}, {7, 2, 0, -1, 0, 1}, {0, 3, 0, -1, 0, 1}, {7, 2, 0, -1, 0, 1}, {3, 3, 0, -1, 0, 1}, {10, 2, 0, -1, 0, 1}, {4, 3, 0, -1, 0, 1}, {11, 2, 0, -1, 0, 2}}},
    {"Rolling", 16, {{0, 3, 1, -1, 0, 1}, {0, 0, 0, -1, 0, 1}, {0, 2, 1, -1, 0, 1}, {0, 0, 0, -1, 0, 1}, {4, 3, 1, -1, 0, 1}, {4, 0, 0, -1, 0, 1}, {4, 2, 1, -1, 0, 1}, {4, 0, 0, -1, 0, 1}, {3, 3, 1, -1, 0, 1}, {3, 0, 0, -1, 0, 1}, {3, 2, 1, -1, 0, 1}, {3, 0, 0, -1, 0, 1}, {4, 3, 1, -1, 0, 1}, {4, 0, 0, -1, 0, 1}, {6, 2, 1, -1, 0, 1}, {7, 2, 1, -1, 1, 1}}},
    {"Rise", 16, {{0, 3, 1, 0, 0, 1}, {2, 2, 1, 0, 0, 1}, {4, 2, 1, 0, 0, 1}, {7, 2, 1, 0, 0, 1}, {2, 1, 1, 0, 0, 1}, {4, 2, 1, 0, 0, 1}, {7, 2, 1, 0, 0, 1}, {9, 2, 1, 0, 0, 1}, {4, 3, 1, 0, 0, 1}, {7, 2, 1, 0, 0, 1}, {9, 2, 1, 0, 0, 1}, {11, 2, 1, 0, 0, 1}, {7, 1, 1, 0, 0, 1}, {9, 2, 1, 0, 0, 1}, {11, 2, 1, 0, 0, 1}, {14, 3, 2, 0, 0, 1}}},
    {"Fall", 16, {{14, 3, 1, 0, 0, 1}, {11, 2, 1, 0, 0, 1}, {9, 2, 1, 0, 0, 1}, {7, 2, 1, 0, 0, 1}, {11, 1, 1, 0, 0, 1}, {9, 2, 1, 0, 0, 1}, {7, 2, 1, 0, 0, 1}, {4, 2, 1, 0, 0, 1}, {9, 3, 1, 0, 0, 1}, {7, 2, 1, 0, 0, 1}, {4, 2, 1, 0, 0, 1}, {2, 2, 1, 0, 0, 1}, {7, 1, 1, 0, 0, 1}, {4, 2, 1, 0, 0, 1}, {2, 2, 1, 0, 0, 1}, {0, 3, 2, 0, 0, 1}}},
    {"Alberti", 8, {{0, 3, 1, 0, 0, 1}, {4, 1, 1, 0, 0, 1}, {2, 2, 1, 0, 0, 1}, {4, 1, 1, 0, 0, 1}, {0, 3, 1, 0, 0, 1}, {4, 1, 1, 0, 0, 1}, {2, 2, 1, 0, 0, 1}, {5, 1, 1, 0, 0, 1}}},
    {"Broken", 8, {{0, 3, 1, 0, 0, 1}, {4, 2, 1, 0, 0, 1}, {2, 2, 1, 0, 0, 1}, {7, 2, 1, 0, 0, 1}, {4, 3, 1, 0, 0, 1}, {9, 2, 1, 0, 0, 1}, {7, 2, 1, 0, 0, 1}, {4, 1, 1, 0, 0, 1}}},
    {"Cascade", 16, {{14, 3, 1, 0, 0, 1}, {11, 2, 1, 0, 0, 1}, {9, 2, 1, 0, 0, 1}, {7, 2, 1, 0, 0, 1}, {11, 3, 1, 0, 0, 1}, {9, 2, 1, 0, 0, 1}, {7, 2, 1, 0, 0, 1}, {4, 2, 1, 0, 0, 1}, {9, 3, 1, 0, 0, 1}, {7, 2, 1, 0, 0, 1}, {4, 2, 1, 0, 0, 1}, {2, 2, 1, 0, 0, 1}, {7, 3, 1, 0, 0, 1}, {4, 2, 1, 0, 0, 1}, {2, 2, 1, 0, 0, 1}, {0, 2, 2, 0, 0, 1}}},
    {"Spread", 8, {{0, 3, 1, 0, 0, 1}, {4, 2, 1, 0, 0, 1}, {9, 2, 1, 0, 0, 1}, {14, 1, 1, 0, 0, 1}, {11, 3, 1, 0, 0, 1}, {7, 2, 1, 0, 0, 1}, {2, 2, 1, 0, 0, 1}, {4, 2, 1, 0, 1, 1}}},
    {"Pedal", 8, {{0, 3, 1, 0, 0, 1}, {2, 2, 1, 0, 0, 1}, {0, 1, 1, 0, 0, 1}, {4, 2, 1, 0, 0, 1}, {0, 1, 1, 0, 0, 1}, {5, 2, 1, 0, 0, 1}, {0, 1, 1, 0, 0, 1}, {7, 2, 1, 0, 0, 1}}},
    {"Arc", 12, {{0, 3, 1, 0, 0, 1}, {2, 2, 1, 0, 0, 1}, {4, 2, 1, 0, 0, 1}, {7, 2, 1, 0, 0, 1}, {9, 2, 1, 0, 0, 1}, {11, 2, 1, 0, 0, 1}, {14, 3, 1, 0, 0, 1}, {11, 2, 1, 0, 0, 1}, {9, 2, 1, 0, 0, 1}, {7, 2, 1, 0, 0, 1}, {4, 2, 1, 0, 0, 1}, {2, 2, 1, 0, 0, 1}}},
    {"Pulse", 8, {{0, 3, 1, 0, 0, 1}, {0, 1, 0, 0, 0, 1}, {4, 2, 0, 0, 0, 1}, {0, 1, 0, 0, 0, 1}, {7, 3, 1, 0, 0, 1}, {0, 1, 0, 0, 0, 1}, {4, 2, 0, 0, 0, 2}, {0, 1, 0, 0, 1, 1}}},
    {"Bounce", 8, {{0, 3, 2, 0, 0, 1}, TIE, {7, 2, 0, 0, 0, 1}, {4, 0, 0, 0, 0, 1}, {0, 2, 2, 0, 0, 1}, TIE, {7, 3, 1, 0, 0, 3}, {4, 2, 0, 0, 1, 1}}},
    {"Stab", 16, {{0, 3, 0, 0, 0, 1}, REST, REST, {4, 2, 0, 0, 0, 1}, REST, REST, {7, 3, 0, 0, 0, 1}, REST, {0, 1, 0, 0, 0, 1}, REST, {4, 2, 0, 0, 0, 1}, REST, {9, 3, 0, 0, 0, 2}, REST, {7, 2, 0, 0, 1, 1}, REST}},
    {"Gate", 16, {{0, 3, 1, 0, 0, 1}, {0, 1, 0, 0, 0, 1}, REST, {0, 2, 0, 0, 0, 1}, {0, 1, 0, 0, 0, 1}, REST, {4, 2, 0, 0, 0, 1}, {0, 1, 0, 0, 0, 1}, {0, 3, 1, 0, 0, 1}, {0, 1, 0, 0, 0, 1}, REST, {0, 2, 0, 0, 0, 1}, {7, 2, 0, 0, 0, 1}, REST, {4, 2, 0, 0, 0, 1}, {0, 1, 0, 0, 1, 1}}},
    {"Dub", 8, {{0, 3, 2, 0, 0, 1}, TIE, TIE, {2, 1, 0, 0, 0, 1}, {4, 2, 2, 0, 0, 1}, TIE, {2, 1, 1, 0, 0, 3}, REST}},
    {"Offbeat", 8, {REST, {0, 3, 0, 0, 0, 1}, REST, {2, 2, 0, 0, 0, 1}, REST, {4, 3, 0, 0, 0, 1}, REST, {2, 2, 0, 0, 1, 1}}},
    {"Hook", 16, {{4, 3, 1, 0, 0, 1}, {2, 2, 1, 0, 0, 1}, {0, 2, 1, 0, 0, 1}, {2, 2, 2, 0, 0, 1}, TIE, {4, 2, 1, 0, 0, 1}, {7, 2, 1, 0, 0, 1}, {4, 2, 2, 0, 0, 1}, TIE, {2, 2, 1, 0, 0, 1}, {0, 2, 1, 0, 0, 1}, {1, 2, 1, 0, 0, 1}, {2, 3, 2, 0, 0, 1}, TIE, TIE, REST}},
    {"Call", 16, {{0, 3, 1, 0, 0, 1}, {2, 2, 1, 0, 0, 1}, {4, 2, 1, 0, 0, 1}, REST, {4, 2, 1, 0, 0, 1}, {5, 2, 1, 0, 0, 1}, {7, 3, 2, 0, 0, 1}, TIE, {7, 2, 1, 0, 0, 1}, {5, 2, 1, 0, 0, 1}, {4, 2, 1, 0, 0, 1}, REST, {2, 2, 1, 0, 0, 1}, {1, 2, 1, 0, 0, 1}, {0, 3, 2, 0, 0, 1}, TIE}},
    {"Sparkle", 16, {{7, 3, 0, 0, 0, 1}, {9, 2, 0, 0, 1, 1}, {11, 2, 0, 0, 0, 1}, {14, 2, 0, 0, 1, 1}, {9, 3, 0, 0, 0, 1}, {11, 2, 0, 0, 1, 1}, {7, 2, 0, 0, 0, 1}, {4, 2, 0, 0, 1, 1}, {7, 3, 0, 0, 0, 1}, {11, 2, 0, 0, 1, 1}, {9, 2, 0, 0, 0, 1}, {14, 2, 0, 0, 1, 1}, {11, 3, 0, 0, 0, 1}, {9, 2, 0, 0, 1, 1}, {7, 2, 0, 0, 0, 1}, {4, 2, 0, 0, 0, 1}}},
    {"Fill", 8, {{0, 2, 1, 0, 0, 2}, {2, 2, 1, 0, 0, 2}, {4, 2, 1, 0, 0, 2}, {7, 2, 1, 0, 0, 2}, {9, 2, 1, 0, 0, 3}, {11, 2, 1, 0, 0, 3}, {14, 3, 1, 0, 0, 3}, REST}},
};
#undef REST
#undef TIE

const int kStepTicks[Arpeggiator::kNumRates] = {96, 48, 32, 24, 16, 12};
const char* const kRateNames[Arpeggiator::kNumRates] = {"1/4", "1/8", "1/8T", "1/16", "1/16T", "1/32"};

struct Scale
{
    const char* name;
    uint16_t    mask; // bit n = semitone n above the key is in the scale
};
constexpr uint16_t M(std::initializer_list<int> notes)
{
    uint16_t m = 0;
    for(int n : notes)
        m |= (uint16_t)(1u << n);
    return m;
}
const Scale kScales[Arpeggiator::kNumScales] = {
    {"Major", M({0, 2, 4, 5, 7, 9, 11})},
    {"Minor", M({0, 2, 3, 5, 7, 8, 10})},
    {"Dorian", M({0, 2, 3, 5, 7, 9, 10})},
    {"Mixolydian", M({0, 2, 4, 5, 7, 9, 10})},
    {"Harm Minor", M({0, 2, 3, 5, 7, 8, 11})},
    {"Pent Major", M({0, 2, 4, 7, 9})},
    {"Pent Minor", M({0, 3, 5, 7, 10})},
    {"Blues", M({0, 3, 5, 6, 7, 10})},
};
const char* const kKeyNames[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};

// Generator shapes. Most are a list of moves (times Jump) round the pool;
// Down starts at the top; Up/Dn bounces off the ends; In/Out, Pedal and
// Scatter are worked out from the move number.
enum Shape
{
    kUp,
    kDown,
    kUpDown,
    k3f2b,
    k2f1b,
    kZigzag,
    kInOut,
    kPedal,
    kScatter
};
const char* const kShapeNames[Arpeggiator::kNumShapes]
    = {"Up", "Down", "Up/Dn", "3+2-", "2+1-", "Zigzag", "In/Out", "Pedal", "Scatter"};
struct Moves
{
    int n;
    int8_t m[5];
};
const Moves kMoves[Arpeggiator::kNumShapes] = {
    {1, {1}}, {1, {-1}}, {1, {1}}, {5, {1, 1, 1, -1, -1}}, {3, {1, 1, -1}}, {2, {2, -1}},
    {0, {}},  {0, {}},   {0, {}},
};

uint32_t Hash(uint32_t x)
{
    x ^= x >> 16;
    x *= 0x7feb352du;
    x ^= x >> 15;
    x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}
int Wrap(int i, int n)
{
    return ((i % n) + n) % n;
}
// The scale note nearest n (key: the scale's root, mask: ScaleMask()); half
// way between two, the one above.
int Snap(int n, int key, uint16_t mask)
{
    for(int d = 0; d < 12; d++)
    {
        if(mask & (1u << Wrap(n + d - key, 12)))
            return n + d;
        if(mask & (1u << Wrap(n - d - key, 12)))
            return n - d;
    }
    return n;
}
} // namespace

const Arpeggiator::Phrase& Arpeggiator::GetPhrase(int i)
{
    return kPhrases[i >= 0 && i < kNumPhrases ? i : 0];
}
const char* Arpeggiator::RateName(int r)
{
    return r >= 0 && r < kNumRates ? kRateNames[r] : "?";
}
const char* Arpeggiator::ScaleName(int s)
{
    return s >= 0 && s < kNumScales ? kScales[s].name : "?";
}
uint16_t Arpeggiator::ScaleMask(int s)
{
    return kScales[s >= 0 && s < kNumScales ? s : 0].mask;
}
int Arpeggiator::ScaleLen(int s)
{
    const uint16_t m = ScaleMask(s);
    int            n = 0;
    for(int i = 0; i < 12; i++)
        n += (m >> i) & 1;
    return n;
}
const char* Arpeggiator::KeyName(int k)
{
    return kKeyNames[((k % 12) + 12) % 12];
}
const char* Arpeggiator::ShapeName(int s)
{
    return s >= 0 && s < kNumShapes ? kShapeNames[s] : "?";
}

const char* Arpeggiator::ChordName(int c)
{
    static const char* const kNames[kNumChords] = {"Triad", "7th", "Power", "Scale", "Sus2", "Sus4"};
    return c >= 0 && c < kNumChords ? kNames[c] : "?";
}

uint8_t Arpeggiator::NextChord(int c)
{
    static const uint8_t kOrder[kNumChords] = {kTriad, kSeventh, kSus2, kSus4, kPower, kScaleRun};
    for(int i = 0; i < kNumChords; i++)
        if(kOrder[i] == c)
            return kOrder[(i + 1) % kNumChords];
    return kTriad;
}

int Arpeggiator::PoolNotes(const Settings& s, int from, int* out)
{
    const uint16_t mask  = ScaleMask(s.scale);
    const int      range = s.range < 1 ? 1 : s.range > kMaxRange ? kMaxRange : s.range;
    // Follow: the scale from the key pressed. Lock: the arp's own key --
    // from the key pressed (or the scale note nearest it).
    const int key  = s.lock ? (int)s.key : from;
    const int root = Snap(from, key, mask);
    auto in = [&](int semis) { return (mask >> Wrap(root + semis - key, 12)) & 1; };

    // An octave of it, as semitones above the root: the scale notes, or a
    // chord's -- each tone the first of its choices that's in the scale
    // (so it comes out major, minor or diminished as the scale says; no
    // third means a sus chord), skipped if none are.
    int oct[12], len = 0;
    if(s.chord == kScaleRun)
    {
        for(int i = 0; i < 12; i++)
            if(in(i))
                oct[len++] = i;
    }
    else
    {
        // Sus: the second or fourth in place of the third (whichever the
        // scale has -- a fourth may be sharp, F B C in C major).
        static const int8_t kThird[] = {4, 3, 5, 2}, kFifth[] = {7, 6, 8}, kSev[] = {10, 11, 9};
        static const int8_t kSecond[] = {2, 1}, kFourth[] = {5, 6};
        oct[len++] = 0;
        auto pick  = [&](const int8_t* c, int n) {
            // The first choice in the scale and above the tone before it --
            // a stand-in fourth and fifth can't both land on one note.
            for(int i = 0; i < n; i++)
                if(in(c[i]) && c[i] > oct[len - 1])
                {
                    oct[len++] = c[i];
                    return;
                }
        };
        if(s.chord == kSus2)
            pick(kSecond, 2);
        else if(s.chord == kSus4)
            pick(kFourth, 2);
        else if(s.chord != kPower)
            pick(kThird, 4);
        pick(kFifth, 3);
        if(s.chord == kSeventh)
            pick(kSev, 3);
    }
    int n = 0;
    for(int o = 0; o < range; o++)
        for(int i = 0; i < len; i++)
            out[n++] = root + 12 * o + oct[i];
    out[n++] = root + 12 * range;
    for(int i = 0; i < n; i++)
    {
        while(out[i] > 127)
            out[i] -= 12;
        while(out[i] < 0)
            out[i] += 12;
    }
    return n;
}

namespace
{
// Chords Follow knows, as pitch classes above the root, best first.
struct ChordShape
{
    uint16_t    mask;
    const char* suffix;
};
constexpr uint16_t P(std::initializer_list<int> pcs)
{
    uint16_t m = 0;
    for(int n : pcs)
        m |= (uint16_t)(1u << n);
    return m;
}
const ChordShape kChordShapes[] = {
    {P({0, 4, 7}), ""},          {P({0, 3, 7}), "m"},         {P({0, 4, 7, 10}), "7"},
    {P({0, 4, 7, 11}), "maj7"},  {P({0, 3, 7, 10}), "m7"},    {P({0, 3, 6}), "dim"},
    {P({0, 3, 6, 10}), "m7b5"},  {P({0, 3, 6, 9}), "dim7"},   {P({0, 4, 8}), "aug"},
    {P({0, 5, 7}), "sus4"},      {P({0, 2, 7}), "sus2"},      {P({0, 4, 7, 9}), "6"},
    {P({0, 3, 7, 9}), "m6"},     {P({0, 3, 7, 11}), "mMaj7"}, {P({0, 4, 7, 2}), "add9"},
    {P({0, 3, 7, 2}), "madd9"},  {P({0, 5, 7, 10}), "7sus4"}, {P({0, 4, 10}), "7"},
    {P({0, 3, 10}), "m7"},       {P({0, 4, 11}), "maj7"},     {P({0, 7}), "5"},
    {P({0, 4}), ""},             {P({0, 3}), "m"},
};
constexpr int kNumChordShapes = sizeof(kChordShapes) / sizeof(kChordShapes[0]);
uint16_t Rotate(uint16_t pcs, int root) // pitch classes as intervals above root
{
    return (uint16_t)(((pcs >> root) | (pcs << (12 - root))) & 0xfff);
}
} // namespace

bool Arpeggiator::FindChord(uint16_t pcs, int* root, int* quality)
{
    // The first shape (best first) that some note of it is the root of.
    for(int q = 0; q < kNumChordShapes; q++)
        for(int r = 0; r < 12; r++)
            if(((pcs >> r) & 1) && Rotate(pcs, r) == kChordShapes[q].mask)
            {
                *root    = r;
                *quality = q;
                return true;
            }
    return false;
}

void Arpeggiator::HeldChordName(int e, char* out, int size) const
{
    if(size <= 0)
        return;
    out[0] = 0;
    const State& a = arps_[e];
    if(a.chord_count < 2)
        return;
    uint16_t pcs = 0;
    int      low = -1;
    for(int n = 0; n < 128; n++)
        if(a.chord[n])
        {
            pcs |= (uint16_t)(1u << (n % 12));
            if(low < 0)
                low = n;
        }
    int r, q;
    if(!FindChord(pcs, &r, &q))
    {
        snprintf(out, (size_t)size, "%s?", KeyName(low));
        return;
    }
    if(low % 12 != r) // an inversion: the bass note after a slash
        snprintf(out, (size_t)size, "%s%s/%s", KeyName(r), kChordShapes[q].suffix, KeyName(low));
    else
        snprintf(out, (size_t)size, "%s%s", KeyName(r), kChordShapes[q].suffix);
}

int Arpeggiator::CurrentPool(int e, int* out) const
{
    const State&    a = arps_[e];
    const Settings& s = Get(e);
    if(a.chord_count < 2)
        return a.root >= 0 ? PoolNotes(s, a.root, out) : 0;
    // The chord's notes in an octave, lowest first (up to 12).
    int notes[12], len = 0, low = -1;
    for(int n = 0; n < 128 && len < 12; n++)
        if(a.chord[n])
        {
            if(low < 0)
                low = n;
            notes[len++] = n;
        }
    if(s.lock)
    {
        // Each onto the scale (the nearest scale note), no doubles.
        const uint16_t mask = ScaleMask(s.scale);
        int            k    = 0;
        for(int i = 0; i < len; i++)
        {
            const int n = Snap(notes[i], s.key, mask);
            if(k == 0 || n != notes[k - 1])
                notes[k++] = n;
        }
        len = k;
    }
    else
    {
        // Follow: whatever the inversion, the chord from its root -- the
        // root at or below the lowest key, its other notes stacked above.
        uint16_t pcs = 0;
        for(int i = 0; i < len; i++)
            pcs |= (uint16_t)(1u << (notes[i] % 12));
        int r, q;
        if(FindChord(pcs, &r, &q))
        {
            const int      base = low - Wrap(low - r, 12);
            const uint16_t iv   = Rotate(pcs, r);
            len                 = 0;
            for(int i = 0; i < 12; i++)
                if((iv >> i) & 1)
                    notes[len++] = base + i;
        }
    }
    const int range = s.range < 1 ? 1 : s.range > kMaxRange ? kMaxRange : s.range;
    int       n     = 0;
    for(int o = 0; o < range; o++)
        for(int i = 0; i < len; i++)
            out[n++] = notes[i] + 12 * o;
    out[n++] = notes[0] + 12 * range;
    for(int i = 0; i < n; i++)
    {
        while(out[i] > 127)
            out[i] -= 12;
        while(out[i] < 0)
            out[i] += 12;
    }
    return n;
}

int Arpeggiator::GenIndex(const Settings& s, int n, int k)
{
    if(n <= 1)
        return 0;
    const int rep  = s.repeat < 1 ? 1 : s.repeat;
    const int jump = s.jump < 1 ? 1 : s.jump;
    const int m    = k / rep; // moves made by step k
    switch(s.shape)
    {
        case kInOut: // bottom, top, next up, next down...
            return (m & 1) ? Wrap(n - 1 - (m / 2) * jump, n) : Wrap((m / 2) * jump, n);
        case kPedal: // the root between every note, climbing
            return (m & 1) ? 1 + Wrap((m / 2) * jump, n - 1) : 0;
        case kScatter: return m == 0 ? 0 : (int)(Hash(0x5ca7u + (uint32_t)m * 977u) % (uint32_t)n);
        case kUpDown:
        {
            // Up and back down. A jump that would pass the top or bottom
            // stops on it and turns -- bouncing back off the end could
            // land on the note just played, a hiccup in the pattern.
            int pos = 0, dir = 1;
            for(int i = 0; i < m; i++)
            {
                if((dir > 0 && pos == n - 1) || (dir < 0 && pos == 0))
                    dir = -dir;
                pos += dir * jump;
                if(pos >= n - 1)
                    pos = n - 1;
                else if(pos <= 0)
                    pos = 0;
            }
            return pos;
        }
        default:
        {
            const Moves& mv  = kMoves[s.shape < kNumShapes ? s.shape : 0];
            int          pos = s.shape == kDown ? n - 1 : 0;
            for(int i = 0; i < m; i++)
                pos = Wrap(pos + mv.m[i % mv.n] * jump, n);
            return pos;
        }
    }
}

Arpeggiator::Flavour Arpeggiator::StepFlavour(int variation, int k)
{
    Flavour f{0, 1.f, 0, false, 1};
    if(variation <= 0)
    {
        f.velocity = (k % 4) == 0 ? 12 : 0; // the beat a little stronger
        return f;
    }
    const uint32_t h = Hash((uint32_t)variation * 0x9e3779b9u + (uint32_t)k * 7919u);
    static const int8_t kVel[8]  = {24, 24, 0, 0, 0, -14, -14, -32};
    static const float  kGate[8] = {0.45f, 0.45f, 1.f, 1.f, 1.f, 1.f, 1.5f, 1.9f};
    f.velocity = kVel[h & 7] + ((k % 4) == 0 ? 10 : 0);
    f.gate     = kGate[(h >> 3) & 7];
    const int o = (h >> 6) & 15;
    f.octave   = o == 0 ? 12 : o == 1 ? -12 : 0;
    f.rest     = k != 0 && ((h >> 10) & 15) < 2;
    const int r = (h >> 14) & 15;
    f.ratchet  = r == 0 ? 2 : r == 1 ? 3 : 1;
    return f;
}

int Arpeggiator::StepTicks(const Settings& s) const
{
    return kStepTicks[s.rate < kNumRates ? s.rate : kRate16];
}

int Arpeggiator::PhraseNote(const Settings& s, int root, int degree)
{
    // The scale as intervals above its key, lowest first.
    const uint16_t mask = ScaleMask(s.scale);
    int            sc[12], n = 0;
    for(int i = 0; i < 12; i++)
        if(mask & (1u << i))
            sc[n++] = i;
    if(n == 0)
        return root;
    // The phrase's octave of 7-note steps, and the step within it.
    const int oct = degree >= 0 ? degree / 7 : -((6 - degree) / 7);
    const int r   = degree - 7 * oct;
    // A step in this scale: the same step in a 7-note one; else the note
    // nearest that step's place in a major scale (within the octave, the
    // lower when it's half way) -- so the octave stays the octave.
    static const int8_t kMajorRef[7] = {0, 2, 4, 5, 7, 9, 11};
    int idx_step = 0;
    if(n == 7)
        idx_step = r;
    else
    {
        int best = 0;
        for(int i = 1; i < n; i++)
        {
            const int db = sc[best] - kMajorRef[r], di = sc[i] - kMajorRef[r];
            if((di < 0 ? -di : di) < (db < 0 ? -db : db))
                best = i;
        }
        idx_step = best;
    }
    if(!s.lock)
        return root + sc[idx_step] + 12 * oct; // Follow: the scale from the key played
    // Lock: up the arp's scale from the scale note nearest the key played.
    const int start = Snap(root, s.key, mask);
    const int pc    = Wrap(start - (int)s.key, 12);
    int       idx   = 0;
    while(idx < n - 1 && sc[idx] != pc)
        idx++;
    const int i = idx + idx_step;
    return start + sc[i % n] - sc[idx] + 12 * (i / n + oct);
}

void Arpeggiator::NoteOff(int e)
{
    State& a = arps_[e];
    if(a.sounding >= 0 && note_fn_)
        note_fn_(e, (uint8_t)a.sounding, 0);
    a.sounding = -1;
}

void Arpeggiator::NoteOn(int e, int note, int velocity, uint64_t now, int len)
{
    State& a = arps_[e];
    velocity = velocity < 1 ? 1 : velocity > 127 ? 127 : velocity;
    if(note_fn_)
        note_fn_(e, (uint8_t)note, (uint8_t)velocity);
    a.sounding = note;
    a.off_at   = now + (uint64_t)(len < 1 ? 1 : len);
}

void Arpeggiator::Stop(int e)
{
    NoteOff(e);
    State& a     = arps_[e];
    for(bool& h : a.held)
        h = false;
    for(int n = 0; n < 128; n++)
        a.chord[n] = a.struck[n] = false;
    a.held_count  = 0;
    a.chord_count = a.struck_count = 0;
    a.pressed     = false;
    a.settled     = true;
    a.active     = false;
    a.root       = -1;
    a.last_slot  = -1;
    a.starting   = false;
    a.last_step  = -1;
    a.last_index = -1;
    a.ratchet_left = 0;
}

void Arpeggiator::Inherit(int t, int p)
{
    if(own_[t][p])
        return;
    pattern_[t][p] = pattern_[t][last_[t]];
    own_[t][p]     = true;
}

Arpeggiator::Settings& Arpeggiator::PatternSettings(int t, int p)
{
    Inherit(t, p);
    last_[t] = (uint8_t)p;
    return pattern_[t][p];
}

void Arpeggiator::CopyPattern(int t, int from, int to)
{
    Inherit(t, from);
    pattern_[t][to] = pattern_[t][from];
    own_[t][to]     = true;
}

void Arpeggiator::CopyToAllPatterns(int t, int from)
{
    Inherit(t, from);
    for(int p = 0; p < kPatterns; p++)
    {
        pattern_[t][p] = pattern_[t][from];
        own_[t][p]     = true;
    }
}

void Arpeggiator::SetPlayingPattern(int t, int p)
{
    if(p == cur_[t] || p < 0 || p >= kPatterns)
        return;
    Stop(TrackArp(t));
    Inherit(t, p);
    cur_[t] = (uint8_t)p;
}

void Arpeggiator::TrackLoopStart(int t)
{
    if(t >= 0 && t < kTracks)
        arps_[TrackArp(t)].step = 0;
}

void Arpeggiator::Reset()
{
    stop_all_ = true;
    for(int e = 0; e < kEngines; e++)
        settings_[e] = Settings{};
    for(int t = 0; t < kTracks; t++)
    {
        for(int p = 0; p < kPatterns; p++)
        {
            pattern_[t][p] = Settings{};
            own_[t][p]     = false;
        }
        last_[t] = 0;
    }
}

namespace
{
void ToSave(const Arpeggiator::Settings& s, bool own, Arpeggiator::SaveEntry* o)
{
    *o           = Arpeggiator::SaveEntry{};
    o->on        = s.on;
    o->mode      = s.mode;
    o->rate      = s.rate;
    o->latch     = s.latch;
    o->steps     = s.steps;
    o->jump      = s.jump;
    o->shape     = s.shape;
    o->repeat    = s.repeat;
    o->phrase    = s.phrase;
    o->variation = s.variation;
    o->key       = s.key;
    o->scale     = s.scale;
    o->lock      = s.lock;
    o->range     = s.range;
    o->chord     = s.chord;
    o->own       = own;
    o->gate01    = s.gate01;
    o->swing01   = s.swing01;
    o->octave    = s.octave;
}
uint8_t Fit(uint8_t v, int lo, int hi)
{
    return (uint8_t)(v < lo ? lo : v > hi ? hi : v);
}
void FromSave(const Arpeggiator::SaveEntry& i, Arpeggiator::Settings* s)
{
    using A      = Arpeggiator;
    s->on        = i.on != 0;
    s->mode      = Fit(i.mode, 0, A::kNumModes - 1);
    s->rate      = Fit(i.rate, 0, A::kNumRates - 1);
    s->latch     = i.latch != 0;
    s->steps     = Fit(i.steps, 1, A::kMaxGenSteps);
    s->jump      = Fit(i.jump, 1, A::kMaxJump);
    s->shape     = Fit(i.shape, 0, A::kNumShapes - 1);
    s->repeat    = Fit(i.repeat, 1, A::kMaxRepeat);
    s->phrase    = Fit(i.phrase, 0, A::kNumPhrases - 1);
    s->variation = Fit(i.variation, 0, A::kNumVariations - 1);
    s->key       = Fit(i.key, 0, 11);
    s->scale     = Fit(i.scale, 0, A::kNumScales - 1);
    s->lock      = i.lock != 0;
    s->range     = Fit(i.range, 1, A::kMaxRange);
    s->chord     = Fit(i.chord, 0, A::kNumChords - 1);
    s->gate01    = i.gate01 >= 0.05f && i.gate01 <= 1.f ? i.gate01 : 0.6f;
    s->swing01   = i.swing01 >= 0.f && i.swing01 <= 1.f ? i.swing01 : 0.f;
    s->octave    = (int8_t)(i.octave < -2 ? -2 : i.octave > 2 ? 2 : i.octave);
}
} // namespace

void Arpeggiator::Save(SaveEntry* out) const
{
    for(int e = 0; e < kSavedFirst; e++)
        ToSave(settings_[e], true, &out[e]);
    for(int t = 0; t < kTracks; t++)
        for(int p = 0; p < kPatterns; p++)
            ToSave(pattern_[t][p], own_[t][p], &out[kSavedFirst + t * kPatterns + p]);
    for(int e = kSavedFirst; e < kEngines; e++)
        ToSave(settings_[e], true, &out[kSavedFirst + kTracks * kPatterns + e - kSavedFirst]);
}

void Arpeggiator::Load(const SaveEntry* in, int n)
{
    Reset();
    for(int i = 0; i < n && i < kSaveEntries; i++)
    {
        constexpr int kLater = kSavedFirst + kTracks * kPatterns; // see Save()
        if(i < kSavedFirst)
            FromSave(in[i], &settings_[i]);
        else if(i < kLater)
        {
            const int t = (i - kSavedFirst) / kPatterns, p = (i - kSavedFirst) % kPatterns;
            FromSave(in[i], &pattern_[t][p]);
            own_[t][p] = in[i].own != 0;
        }
        else
            FromSave(in[i], &settings_[kSavedFirst + i - kLater]);
    }
}

void Arpeggiator::StopAll()
{
    for(int e = 0; e < kInstances; e++)
        Stop(e);
}

void Arpeggiator::KeyDown(int e, uint8_t note, uint8_t velocity, bool at_once)
{
    if(e < 0 || e >= kInstances || note > 127)
        return;
    State& a = arps_[e];
    if(!a.held[note])
    {
        a.held[note] = true;
        a.held_count++;
    }
    for(int n = 0; n < 128; n++)
        a.struck[n] = a.held[n];
    a.struck_count = a.held_count;
    a.down_ms[note] = ms_;
    if(!a.pressed)
        a.change_ms = ms_; // the first key of a change
    a.pressed      = true;
    a.changed_ms   = at_once ? ms_ - kSettleMs : ms_;
    a.settled      = false;
    a.velocity     = velocity;
    if(!a.active)
    {
        // Starts from the first step, on the grid (see Tick()). last_slot is
        // kept: the step it last played isn't played again.
        a.active      = true;
        a.starting    = true;
        a.step        = 0;
        a.last_index  = -1;
        a.press_tick  = now_tick_;
        a.start_known = false;
    }
}

void Arpeggiator::KeyUp(int e, uint8_t note, bool at_once)
{
    if(e < 0 || e >= kInstances || note > 127)
        return;
    State& a = arps_[e];
    if(a.held[note])
    {
        a.held[note] = false;
        a.held_count--;
    }
    if(Get(e).latch)
    {
        // Letting go changes nothing -- except a key from the chord before
        // this one, let go after the new one went down (playing legato):
        // it's leaving the chord, not part of the new one.
        if(a.struck[note] && (int32_t)(a.down_ms[note] - a.change_ms) < 0)
        {
            a.struck[note] = false;
            a.struck_count--;
            if(a.chord[note] && a.chord_count > 1)
            {
                a.chord[note] = false;
                a.chord_count--;
            }
        }
        return;
    }
    if(a.held_count == 0)
    {
        NoteOff(e);
        a.active       = false;
        a.ratchet_left = 0;
        a.chord_count  = 0;
        a.settled      = true;
        return;
    }
    a.changed_ms = at_once ? ms_ - kSettleMs : ms_; // what's left, once it settles
    a.settled    = false;
}

bool Arpeggiator::TakeChord(int e, bool final)
{
    // The keys are the chord now. Latch takes the keys struck (a quick
    // stab, already let go, still counts); otherwise the keys down.
    State&     a     = arps_[e];
    const bool latch = Get(e).latch;
    if(latch && !a.pressed)
        return false;
    const bool* src   = latch ? a.struck : a.held;
    const int   count = latch ? a.struck_count : a.held_count;
    if(count == 0)
        return false;
    int low = -1;
    for(int n = 0; n < 128; n++)
    {
        a.chord[n] = src[n];
        if(src[n] && low < 0)
            low = n;
    }
    a.chord_count = count;
    a.root        = low; // one key: it; a chord: its lowest (for Phrase)
    if(final)
        a.pressed = false;
    return true;
}

void Arpeggiator::Tick(uint64_t abs_tick, bool running, uint32_t now_ms)
{
    ms_       = now_ms;
    now_tick_ = abs_tick;
    if(stop_all_)
    {
        stop_all_ = false;
        StopAll();
    }
    for(int e = 0; e < kInstances; e++)
    {
        const Settings& s = Get(e);
        State&          a = arps_[e];
        if(!a.settled && ms_ - a.changed_ms >= kSettleMs)
        {
            a.settled = true;
            TakeChord(e); // for the next step -- the one playing is left be
        }
        if(!s.on)
        {
            if(a.active || a.sounding >= 0)
                Stop(e);
            continue;
        }
        if(!a.active || !running)
        {
            if(a.sounding >= 0 && (!a.active || abs_tick >= a.off_at))
                NoteOff(e);
            continue;
        }
        // The gate ends.
        if(a.sounding >= 0 && abs_tick >= a.off_at)
            NoteOff(e);
        // A ratchet's next hit.
        if(a.ratchet_left > 0 && abs_tick >= a.ratchet_next)
        {
            NoteOff(e);
            NoteOn(e, a.ratchet_note, a.ratchet_vel, abs_tick, a.ratchet_len);
            a.ratchet_left--;
            a.ratchet_next += (uint64_t)a.ratchet_every;
        }

        const int     st    = StepTicks(s);
        const int64_t slot  = (int64_t)(abs_tick / (uint64_t)st);
        const int     phase = (int)(abs_tick % (uint64_t)st);
        if(s.rate != a.rate_used)
        {
            // A new rate: carry on from its next step, rather than a note
            // straight away (the slot numbers all change with it).
            if(a.last_slot >= 0)
                a.last_slot = slot;
            a.rate_used = s.rate;
        }
        // Swing: every second step a little late.
        const int swing_at = (slot & 1) ? (int)(s.swing01 * 0.5f * (float)st) : 0;
        if(a.starting && !a.start_known)
        {
            // The first step at or after the key went down (one on the
            // tick it went down counts), past the one last played.
            uint64_t b = (a.press_tick + (uint64_t)st - 1) / (uint64_t)st * (uint64_t)st;
            if(a.last_slot >= 0 && (int64_t)(b / (uint64_t)st) <= a.last_slot)
                b = (uint64_t)(a.last_slot + 1) * (uint64_t)st;
            a.start_at    = b;
            a.start_known = true;
        }
        const bool due = (!a.starting || abs_tick >= a.start_at) && slot != a.last_slot && phase >= swing_at;
        if(!due)
            continue;
        a.starting = false;
        // Keys still landing: this step plays with what's down already (the
        // chord's taken for good once they settle, from the next step).
        if(!a.settled)
            TakeChord(e, false);
        if(a.chord_count == 0)
            continue;
        a.last_slot = slot;
        const int len_steps = s.mode == kPhrase ? GetPhrase(s.phrase).len : (s.steps < 1 ? 1 : s.steps);
        if(a.step >= len_steps)
            a.step = 0;
        const int k    = a.step;
        a.step         = (a.step + 1) % len_steps;
        if(k == 0)
            a.cycle++;
        a.last_step    = k;
        a.index_before = a.last_index;
        PlayStep(e, k, abs_tick, st);
    }
}

void Arpeggiator::PlayStep(int e, int k, uint64_t abs_tick, int st)
{
    const Settings& s = Get(e);
    State&          a = arps_[e];
    const Flavour f   = StepFlavour(s.variation, k);

    int   note = -1, ties = 0, vel_add = 0, ratchet = f.ratchet;
    float len_scale = 1.f; // of the gate; -1 = the whole step (legato)
    bool  rest      = f.rest;
    int   oct_add   = f.octave;
    if(s.mode == kPhrase)
    {
        // The phrase's own dynamics; a Variation only nudges its velocity
        // and gate (its rests, octave jumps and ratchets are the phrase's).
        const Phrase& ph   = GetPhrase(s.phrase);
        const Step&   step = ph.steps[k < ph.len ? k : 0];
        if(step.degree == kTie)
            return; // the note before carries on (its gate covered this)
        rest    = step.degree == kRest;
        oct_add = 0;
        ratchet = step.ratchet < 1 ? 1 : step.ratchet;
        if(!rest)
        {
            static const int8_t kVel[4] = {-45, -22, 0, 25};
            note = PhraseNote(s, a.root, step.degree) + 12 * (step.octave + s.octave);
            while(note < 0)
                note += 12;
            while(note > 127)
                note -= 12;
            vel_add = kVel[step.velocity & 3];
            if(step.length == 0)
                len_scale = 0.5f;
            for(int j = (k + 1) % ph.len; ties < ph.len && ph.steps[j].degree == kTie; j = (j + 1) % ph.len)
                ties++;
            if(step.length == 2 || ties > 0)
                len_scale = -1.f;
            // A chance step: a fixed throw per time round, so the same start
            // plays the same.
            if(step.chance && (Hash(a.cycle * 131u + (uint32_t)k * 7u + 0x51u) & 1u))
                rest = true;
        }
    }
    else
    {
        int       pool[kMaxPool];
        const int n   = CurrentPool(e, pool);
        int       idx = GenIndex(s, n, k);
        // Looping round onto the note just played would stutter: the
        // first step takes the next note up instead.
        if(k == 0 && idx == a.index_before && n > 1 && s.repeat <= 1)
            idx = idx + 1 < n ? idx + 1 : idx - 1;
        a.last_index = idx;
        note         = pool[idx];
    }
    if(note >= 0 && !rest)
    {
        note += oct_add;
        if(note < 0 || note > 127)
            note -= oct_add;
    }
    a.ratchet_left = 0;
    NoteOff(e);
    if(note < 0 || rest)
        return;
    const int vel = a.velocity + f.velocity + vel_add;
    const float g = len_scale < 0.f ? 1.f : s.gate01 * f.gate * len_scale;
    if(ratchet > 1)
    {
        // Hits evenly through the step, each short.
        const int every = st / ratchet;
        const int len   = (int)((float)every * (g < 0.9f ? g : 0.9f));
        NoteOn(e, note, vel, abs_tick, len);
        a.ratchet_left  = ratchet - 1;
        a.ratchet_every = every;
        a.ratchet_next  = abs_tick + (uint64_t)every;
        a.ratchet_len   = len;
        a.ratchet_note  = note;
        a.ratchet_vel   = (uint8_t)(vel * 4 / 5 < 1 ? 1 : vel * 4 / 5 > 127 ? 127 : vel * 4 / 5);
        return;
    }
    // Held through any ties that follow, then the gate's share of the last
    // step.
    NoteOn(e, note, vel, abs_tick, (int)((float)st * ((float)ties + g)));
}

namespace
{
// Render()'s note collector (main loop only, one render at a time). Ticks
// are from the render's start; notes begun in the kept lap are kept, and
// ended wherever they end -- in the lap after it if they run on.
Arpeggiator::RenderedNote* g_render_out   = nullptr;
int                        g_render_max   = 0;
int                        g_render_count = 0;
bool                       g_render_keep  = false; // in the lap that's kept
uint64_t                   g_render_tick  = 0;
uint64_t                   g_render_from  = 0;     // where the kept lap starts
uint64_t                   g_render_on[128];
uint8_t                    g_render_vel[128];
bool                       g_render_open[128];
void RenderClose(int note)
{
    g_render_open[note] = false;
    if(g_render_count < g_render_max)
        g_render_out[g_render_count++] = {(uint32_t)(g_render_on[note] - g_render_from),
                                          (uint32_t)(g_render_tick - g_render_on[note]), (uint8_t)note,
                                          g_render_vel[note]};
}
void RenderNote(int, uint8_t note, uint8_t velocity)
{
    if(g_render_open[note])
        RenderClose(note); // struck again (or let go)
    if(velocity > 0 && g_render_keep)
    {
        g_render_on[note]   = g_render_tick;
        g_render_vel[note]  = velocity;
        g_render_open[note] = true;
    }
}
} // namespace

int Arpeggiator::Render(const Settings& s, const KeyEvent* ev, int n, uint32_t len, RenderedNote* out, int max)
{
    if(len == 0 || !out || max <= 0)
        return 0;
    // A clean track arp 0 playing `s`, collecting what it plays.
    Init(&RenderNote);
    for(int i = 0; i < kInstances; i++)
        arps_[i] = State{};
    stop_all_ = false;
    ms_       = 0;
    now_tick_ = 0;
    for(int p = 0; p < kPatterns; p++)
        own_[0][p] = false;
    pattern_[0][0] = s;
    own_[0][0]     = true;
    cur_[0]        = 0;
    const int arp  = TrackArp(0);
    g_render_out   = out;
    g_render_max   = max;
    g_render_count = 0;
    g_render_from  = 2ull * len;
    for(bool& o : g_render_open)
        o = false;
    // Laps as main.cpp drives a playing track: each tick the sequencer's
    // notes (straight to the arp, together), the top of the track
    // restarting the pattern, then Tick(). The third is kept -- by then
    // latch and the steps are where a playing track's would be, and two
    // laps in, its swing falls as on the loop -- and a fourth only ends the
    // third's notes that run on (as the next lap's first step would).
    for(int lap = 0; lap < 4; lap++)
    {
        g_render_keep = lap == 2;
        int next      = 0;
        for(uint32_t t = 0; t < len; t++)
        {
            const uint64_t abs = (uint64_t)lap * len + t;
            g_render_tick      = abs;
            if(t == 0 && lap > 0)
                TrackLoopStart(0);
            for(; next < n && ev[next].tick <= t; next++)
            {
                if(ev[next].tick < t)
                    continue;
                if(ev[next].velocity > 0)
                    KeyDown(arp, ev[next].note, ev[next].velocity, true);
                else
                    KeyUp(arp, ev[next].note, true);
            }
            Tick(abs, true, (uint32_t)abs);
            if(lap == 3)
            {
                bool any = false;
                for(bool o : g_render_open)
                    any = any || o;
                if(!any)
                    break; // the kept lap's notes have all ended
            }
        }
    }
    // Anything still open (a very long tie): ended a lap after it began.
    g_render_tick = 4ull * len;
    for(int k = 0; k < 128; k++)
        if(g_render_open[k])
            RenderClose(k);
    note_fn_ = nullptr;
    return g_render_count;
}

