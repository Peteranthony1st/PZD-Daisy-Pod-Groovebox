#pragma once
// Host stand-in for the CMSIS bits the firmware touches (the profiler's
// cycle counter, interrupt masking): harmless no-ops.
#include <cstdint>
struct HostDwt
{
    uint32_t CTRL, CYCCNT, LAR;
};
struct HostCoreDebug
{
    uint32_t DEMCR;
};
inline HostDwt       g_host_dwt;
inline HostCoreDebug g_host_coredebug;
#define DWT (&g_host_dwt)
#define CoreDebug (&g_host_coredebug)
#define DWT_CTRL_CYCCNTENA_Msk 1u
#define CoreDebug_DEMCR_TRCENA_Msk 1u
inline void __disable_irq() {}
inline void __enable_irq() {}
inline uint32_t __get_PRIMASK() { return 0; }
inline void     __set_PRIMASK(uint32_t) {}
inline int32_t  __SSAT(int32_t v, uint32_t bits)
{
    const int32_t hi = (1 << (bits - 1)) - 1, lo = -(1 << (bits - 1));
    return v > hi ? hi : (v < lo ? lo : v);
}
