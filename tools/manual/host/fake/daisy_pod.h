#pragma once
#include "daisy_seed.h"

namespace daisy
{
class DaisyPod
{
  public:
    enum Knob
    {
        KNOB_1,
        KNOB_2,
        KNOB_LAST
    };
    float GetKnobValue(Knob k) const { return knob[k]; }
    Switch  button1, button2;
    Encoder encoder;
    RgbLed  led1, led2;
    DaisySeed seed;
    float   knob[2] = {0.f, 0.f};
};
} // namespace daisy
