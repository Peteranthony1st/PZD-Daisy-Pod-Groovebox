#pragma once

#include <cstdint>

// Fixed 1-bit artwork for the OLED: the power-on splash and the graphic
// on each Global engine/Looper entry page. All of it is row-major,
// MSB = leftmost pixel, (w + 7) / 8 bytes per row -- see
// Ui::DrawBitmap(). ui_graphics.cpp is generated from the approved
// mockups rather than drawn by hand.

namespace daisy
{
class OneBitGraphicsDisplay;
}

// Global page graphics: all the same size, drawn centred between the
// Enabled/Disabled line and the "PUSH ENC TO OPEN" hint.
constexpr int kGlobalIconW     = 48;
constexpr int kGlobalIconH     = 20;
constexpr int kGlobalIconBytes = (kGlobalIconW + 7) / 8 * kGlobalIconH;

extern const uint8_t kGlobalIconLooper[kGlobalIconBytes];   // tape reels
extern const uint8_t kGlobalIconGrains[kGlobalIconBytes];   // play head, grains behind
extern const uint8_t kGlobalIconDexedA[kGlobalIconBytes];   // DX7-style algorithm, A badge
extern const uint8_t kGlobalIconDexedB[kGlobalIconBytes];   // ... and B badge
extern const uint8_t kGlobalIconEnsemble[kGlobalIconBytes]; // registration tabs
extern const uint8_t kGlobalIconOrgan[kGlobalIconBytes];    // nine drawbars on a rail
extern const uint8_t kGlobalIconSupersaw[kGlobalIconBytes]; // three detuned saws
extern const uint8_t kGlobalIconSmpvo[kGlobalIconBytes];    // one-octave keyboard
extern const uint8_t kGlobalIconSmpdr[kGlobalIconBytes];    // 4x2 drum pads
extern const uint8_t kGlobalIconExport[kGlobalIconBytes];   // waveform into an SD card

// Full-screen power-on splash: PZD logo with GROOVEBOX underneath.
constexpr int kSplashW     = 128;
constexpr int kSplashH     = 64;
constexpr int kSplashBytes = (kSplashW + 7) / 8 * kSplashH;

extern const uint8_t kSplashBitmap[kSplashBytes];

// Draws a bitmap in the format above with its top-left at (x, y). Only
// set pixels are drawn, so it overlays whatever is already there.
void DrawBitmap(daisy::OneBitGraphicsDisplay* disp,
                int                           x,
                int                           y,
                int                           w,
                int                           h,
                const uint8_t*                bits);
