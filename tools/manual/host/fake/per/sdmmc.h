#pragma once
#include "ff.h"
namespace daisy
{
class SdmmcHandler
{
  public:
    struct Config
    {
        enum class Speed
        {
            SLOW,
            MEDIUM_SLOW,
            STANDARD,
            FAST,
            VERY_FAST
        };
        Speed speed = Speed::FAST;
        int   width = 4;
        bool  clock_powersave = false;
        void  Defaults() {}
    };
    using Speed = Config::Speed;
    int Init(const Config&) { return 0; }
};
} // namespace daisy
