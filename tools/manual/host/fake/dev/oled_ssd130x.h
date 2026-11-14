#pragma once
// Host stand-in: the real drawing code (OneBitGraphicsDisplayImpl), over a
// plain 128x64 framebuffer the renderer saves as an image.
#include <cstring>
#include "daisy_seed.h"
#include "hid/disp/display.h"

namespace daisy
{
class HostDisplay : public OneBitGraphicsDisplayImpl<HostDisplay>
{
  public:
    uint16_t Height() const override { return 64; }
    uint16_t Width() const override { return 128; }
    void     Fill(bool on) override { memset(px, on ? 1 : 0, sizeof(px)); }
    void     DrawPixel(uint_fast8_t x, uint_fast8_t y, bool on) override
    {
        if(x < 128 && y < 64)
            px[y][x] = on ? 1 : 0;
    }
    void Update() override {}
    bool UpdateFinished() override { return true; }
    uint8_t px[64][128] = {};
};
} // namespace daisy
