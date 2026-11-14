#pragma once
#include "ff.h"
namespace daisy
{
class FatFSInterface
{
  public:
    struct Config
    {
        enum Media
        {
            MEDIA_SD = 1,
            MEDIA_USB = 2
        };
        uint8_t media = MEDIA_SD;
    };
    int         Init(const Config&) { return 0; }
    int         Init(uint8_t) { return 0; }
    FATFS&      GetSDFileSystem() { return fs_; }
    const char* GetSDPath() const { return "0:/"; }
  private:
    FATFS fs_;
};
} // namespace daisy
