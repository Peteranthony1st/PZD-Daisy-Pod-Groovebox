// The LFO and LFO Depth pages, shared by every engine with a SynthLfo (the
// Supersaw and the Ensemble): LFO -- K1 Rate (a note value when synced),
// K2 Fade, B1 cycles the wave, B2 Free/Sync; LFO Depth -- K1 picks
// Pitch/Filter/Amp, K2 sets that one's depth (bipolar).
#include "ui.h"
#include "synth_lfo.h"
#include <cstdio>

using namespace daisy;

namespace
{
inline float Clamp(float v, float lo, float hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

const char* const kWaves[] = {"Tri", "Saw", "Square", "S&H"};
const char* const kDests[] = {"Pitch", "Filter", "Amp"};
} // namespace

void Ui::LfoSyncPickup(const SynthLfo& lfo, bool depth_page, int dest, size_t i)
{
    if(depth_page)
    {
        // K1 picks the destination directly (no pickup).
        k2_pickup_raw_[i] = lfo.GetDepth01(dest);
        return;
    }
    k1_pickup_raw_[i] = lfo.GetSync()
                            ? ((float)lfo.GetSyncIndex() + 0.5f) / (float)SynthLfo::kNumSyncs
                            : lfo.GetRate01();
    k2_pickup_raw_[i] = lfo.GetFade01();
}

void Ui::LfoApplyKnobs(SynthLfo& lfo, bool depth_page, int* dest, float k1, float k2, size_t ci)
{
    if(depth_page)
    {
        // K1 browses Pitch/Filter/Amp directly, like the Ensemble's drawbar
        // select; K2 then edits that one's depth.
        const int d = (int)(Clamp(k1, 0.f, 0.999f) * (float)SynthLfo::kNumDests);
        if(d != *dest)
        {
            *dest                  = d;
            k2_pickup_engaged_[ci] = false;
            k2_pickup_raw_[ci]     = lfo.GetDepth01(d);
        }
        if(PickUpK2(k2))
            lfo.SetDepth01(*dest, k2);
        return;
    }
    if(PickUpK1(k1))
    {
        if(lfo.GetSync())
            lfo.SetSyncIndex((int)(Clamp(k1, 0.f, 0.999f) * (float)SynthLfo::kNumSyncs));
        else
            lfo.SetRate01(k1);
    }
    if(PickUpK2(k2))
        lfo.SetFade01(k2);
}

void Ui::LfoCycleWave(SynthLfo& lfo)
{
    lfo.SetWave((SynthLfo::Wave)(((int)lfo.GetWave() + 1) % (int)SynthLfo::Wave::kCount));
}

void Ui::LfoToggleSync(SynthLfo& lfo, KnobContext ctx)
{
    // K1 swaps between a rate and a note value: seed its pickup to the
    // new meaning.
    lfo.SetSync(!lfo.GetSync());
    const size_t ci        = (size_t)ctx;
    k1_pickup_engaged_[ci] = false;
    LfoSyncPickup(lfo, false, 0, ci);
}

void Ui::DrawLfoPage(const SynthLfo& lfo)
{
    // Two cycles of its wave (S&H as random steps).
    const int x0 = 4, x1 = disp_->Width() - 5, yt = 14, yb = kFooterDividerY - 4;
    const auto wave = lfo.GetWave();
    static const float kSh[8] = {0.7f, 0.2f, 0.9f, 0.4f, 0.1f, 0.8f, 0.5f, 0.3f};
    int py = -1;
    for(int x = x0; x <= x1; x++)
    {
        float     p   = 2.f * (float)(x - x0) / (float)(x1 - x0 + 1);
        const int cyc = (int)p;
        p -= (float)cyc;
        float y01;
        switch(wave)
        {
            case SynthLfo::Wave::Triangle: y01 = p < 0.5f ? 2.f * p : 2.f - 2.f * p; break;
            case SynthLfo::Wave::Saw: y01 = p; break;
            case SynthLfo::Wave::Square: y01 = p < 0.5f ? 1.f : 0.f; break;
            default: y01 = kSh[(cyc * 4 + (int)(p * 4.f)) & 7]; break;
        }
        const int y = yb - (int)((float)(yb - yt) * y01 + 0.5f);
        disp_->DrawLine(x, py < 0 ? y : py, x, y, true);
        py = y;
    }
    char v1[16], v2[16], line[24];
    if(lfo.GetSync())
        snprintf(v1, sizeof(v1), "%s", SynthLfo::SyncName(lfo.GetSyncIndex()));
    else
    {
        const float hz = lfo.GetRateHz();
        if(hz < 10.f)
            snprintf(v1, sizeof(v1), "%d.%02dHz", (int)hz, (int)(hz * 100.f) % 100);
        else
            snprintf(v1, sizeof(v1), "%dHz", (int)(hz + 0.5f));
    }
    const int cs = (int)(lfo.GetFadeSeconds() * 100.f + 0.5f); // no float printf
    snprintf(v2, sizeof(v2), "%d.%02ds", cs / 100, cs % 100);
    DrawControlRow(kFooterRow1Y, false, kFooterDividerY, lfo.GetSync() ? "Sync" : "Rate", v1, v2,
                   "Fade");
    snprintf(line, sizeof(line), "Wave:%s", kWaves[(int)wave]);
    DrawControlRow(kFooterRow2Y, true, kFooterInterRowDividerY, line, "", "", "");
    DrawStarToggle(false, "Free/Sync", lfo.GetSync());
}

void Ui::DrawLfoDepthPage(const SynthLfo& lfo, int dest)
{
    // A bipolar bar per destination, the chosen one marked.
    const int w = disp_->Width() - 44, mid = 40 + w / 2;
    for(int d = 0; d < SynthLfo::kNumDests; d++)
    {
        const int   y   = 13 + d * 11;
        const float v   = lfo.Depth(d);
        const bool  sel = d == dest;
        if(sel)
            TomThumbDrawText(disp_, 0, y + 6, ">", true);
        TomThumbDrawText(disp_, 6, y + 6, kDests[d], true);
        disp_->DrawRect(40, y, 40 + w - 1, y + 6, true, false);
        disp_->DrawLine(mid, y, mid, y + 6, true);
        const int end = mid + (int)(v * (float)(w / 2 - 1));
        if(end != mid)
            disp_->DrawRect(end < mid ? end : mid, y + 2, end < mid ? mid : end, y + 4, true, true);
    }
    char v2[12];
    const float d01 = lfo.GetDepth01(dest);
    const int   p   = (int)((d01 * 2.f - 1.f) * 100.f + (d01 >= 0.5f ? 0.5f : -0.5f));
    if(p == 0)
        snprintf(v2, sizeof(v2), "0%%");
    else
        snprintf(v2, sizeof(v2), "%+d%%", p);
    DrawControlRow(kFooterRow1Y, false, kFooterDividerY, "Dest", kDests[dest], v2, "Depth");
    DrawControlRow(kFooterRow2Y, true, kFooterInterRowDividerY, "", "", "", "");
}
