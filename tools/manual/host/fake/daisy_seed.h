#pragma once
// The host renderer's stand-in for libDaisy's hardware layer: only what the
// UI and the engines touch, with the clock under the renderer's control.
#include <cstdint>
#include <cstddef>
#include "daisy_core.h"

#define DSY_SDRAM_BSS
#define DSY_QSPI_BSS

#include "stm32h7xx.h"

namespace daisy
{
class AudioHandle
{
  public:
    typedef const float* const* InputBuffer;
    typedef float**             OutputBuffer;
};

class System
{
  public:
    static uint32_t GetNow() { return now_ms; }
    static uint32_t GetUs() { return now_ms * 1000u; }
    static uint32_t GetTick() { return now_ms * 200000u; }
    static void     Delay(uint32_t ms) { now_ms += ms; }
    static void     DelayUs(uint32_t) {}
    static uint32_t now_ms;
};

class Switch
{
  public:
    bool  Pressed() const { return pressed; }
    float TimeHeldMs() const { return pressed ? (float)(System::GetNow() - down_ms) : 0.f; }
    bool  RisingEdge() const { return false; }
    bool  FallingEdge() const { return false; }
    void  Debounce() {}
    bool     pressed = false;
    uint32_t down_ms = 0;
};

class Encoder
{
  public:
    bool  Pressed() const { return sw.Pressed(); }
    float TimeHeldMs() const { return sw.TimeHeldMs(); }
    int32_t Increment() const { return 0; }
    bool  RisingEdge() const { return false; }
    bool  FallingEdge() const { return false; }
    void  Debounce() {}
    Switch sw;
};

class Led
{
  public:
    void Set(float) {}
    void Update() {}
};
class RgbLed
{
  public:
    void Set(float, float, float) {}
    void SetRed(float) {}
    void SetGreen(float) {}
    void SetBlue(float) {}
    void Update() {}
};

class PWMHandle
{
  public:
    struct Config
    {
    };
    class Channel
    {
      public:
        struct Config
        {
        };
        void Set(float v) { value = v; }
        float value = 0.f;
    };
};

class CpuLoadMeter
{
  public:
    void  Init(float, size_t, float = 1.f) {}
    void  OnBlockStart() {}
    void  OnBlockEnd() {}
    float GetAvgCpuLoad() const { return 0.f; }
    float GetMaxCpuLoad() const { return 0.f; }
    float GetMinCpuLoad() const { return 0.f; }
    void  Reset() {}
};

class DaisySeed
{
};
} // namespace daisy
