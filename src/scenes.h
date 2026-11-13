#pragma once
#include <cstdint>
#include "sequencer.h"

// A scene: what plays together -- each MIDI track's pattern (or off) and
// each audio layer on or off, at its own volume. Home's Scenes page
// saves the current state into one and launches it; a launch switches
// everything together where the loop next starts (see
// MidiSequencer::RequestScene(), LooperLayer::QueueScene()).
struct Scene
{
    static constexpr int kLayers = 4;
    uint8_t used = 0;
    int8_t  pattern[MidiSequencer::kTracks] = {-1, -1, -1, -1, -1, -1, -1, -1}; // -1 = off
    uint8_t layer_on[kLayers]    = {};
    float   layer_vol01[kLayers] = {};
    // Each track's chain (MidiSequencer::RequestChain()); < 2 = just pattern[].
    uint8_t chain_len[MidiSequencer::kTracks] = {};
    int8_t  chain[MidiSequencer::kTracks][MidiSequencer::kMaxChain] = {};
};
static_assert(sizeof(Scene) == 104, "Scene is saved as is -- grow it only by appending");
static_assert(MidiSequencer::kMaxChain == 8, "RequestScene() takes chains of 8");
constexpr int kScenes = 16;

// The song: scenes in order, each for a number of loops. Home's Song page
// edits it, plays it (each scene launched for the loop it starts on) and
// records it (the scenes launched while recording, and for how long).
struct SongStep
{
    uint8_t scene   = 0; // 0..kScenes-1
    uint8_t repeats = 1; // loops, 1..Song::kMaxRepeats
};
struct Song
{
    static constexpr int kSteps      = 64;
    static constexpr int kMaxRepeats = 32;
    uint8_t  len         = 0;
    uint8_t  reserved[3] = {};
    SongStep steps[kSteps];
};
static_assert(sizeof(Song) == 132, "Song is saved as is -- grow it only by appending");
