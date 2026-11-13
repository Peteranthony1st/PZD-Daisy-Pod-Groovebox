#pragma once
#include "dev/oled_ssd130x.h"

// libDaisy's SSD1306/SSD1309 I2C driver, plus the panel commands the
// screen saver needs (contrast, panel on/off) -- the stock driver only
// sends them during Init(). OledDisplay keeps its driver private, so the
// one instance registers itself for Command().
class PodOledDriver : public daisy::SSD130xI2c128x64Driver
{
  public:
    void Init(Config config)
    {
        daisy::SSD130xI2c128x64Driver::Init(config);
        instance_ = this;
    }
    // Main loop only, between frames (it shares the display's I2C bus).
    static void Command(uint8_t cmd)
    {
        if(instance_)
            instance_->transport_.SendCommand(cmd);
    }

  private:
    static inline PodOledDriver* instance_ = nullptr;
};
