#pragma once
#include <cstdint>

// Global:Export's Live mode, shared between main.cpp's audio callback
// (which captures the master output) and the Ui (which writes it out).
//
// Nothing touches the SD card while the audio runs -- writing to it then
// isn't reliable on this hardware (see audio_engine.h's g_audio_suspended)
// -- so the capture goes into memory: the looper's undo buffer, lent out
// for the duration (LooperLayer::BlockUndoBackup()), as two segments of
// 16-bit stereo frames. The UI sets loops_target and state = kArmed; the
// callback starts on the next bar 1 (the loop position wrapping round),
// counts loop wraps, and sets kDone after loops_target of them or when the
// memory is full (the UI can also set kDone to stop early). Then the UI
// pauses the audio and saves the WAV, like any other save.
struct LiveExport
{
    enum State : int
    {
        kIdle,
        kArmed,     // waiting for bar 1
        kRecording,
        kDone,      // the UI writes the WAV and goes back to kIdle
    };
    int16_t*          seg[2]       = {}; // interleaved L,R frames (SDRAM)
    uint32_t          seg_frames   = 0;  // frames each segment holds
    volatile int      state        = kIdle;
    volatile uint32_t loops_target = 1;
    volatile uint32_t loops_done   = 0;
    volatile uint32_t frames       = 0; // captured so far
    volatile bool     full         = false; // stopped because memory ran out
    // Song mode: records until the UI says done, however long. When the
    // memory is nearly full the callback raises flush_req; the UI pauses
    // the audio (everything freezes, so the file stays seamless), writes
    // what's there, empties it and carries on. mute keeps the outputs silent.
    volatile bool     song         = false;
    volatile bool     flush_req    = false;
    volatile bool     mute         = false;
    // Room left when flush_req goes up -- the main loop's slack (1s).
    static constexpr uint32_t kFlushMargin = 48000;

    uint32_t Capacity() const { return 2 * seg_frames; }
};
