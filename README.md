<img height="220" alt="IMG_20260829_153832" src="https://github.com/user-attachments/assets/2b5ef4e8-39c5-4f6e-b0f3-ebab1151912c" /><img height="220" alt="IMG_20260829_153742" src="https://github.com/user-attachments/assets/c5dccf4c-f985-4fb2-90eb-8b50f2cbe780" /><img height="220" alt="IMG_20260829_153938" src="https://github.com/user-attachments/assets/f0e90580-c2f8-4c6a-9800-b38b15a6b568" />

Video and looper generated WAV file below!

# PZD-Daisy-Pod-Groovebox

A pocket, **multitimbral** groovebox for the [Electrosmith Daisy
Pod](https://daisy.audio/products/pod): a 4-layer stereo looper plus five
synth/sampler engines, every one of them playable together at the same
time on one small board — not one-at-a-time — each independently
switchable and MIDI-played alongside the looper. An added I2C OLED
display gives it a full on-device menu: live waveform/level display,
per-layer filter/effects/reverb, tempo-locked recording, and SD card
save/load, all controlled from the Pod's two knobs, two buttons, and one
clickable encoder.

## Engines

- **Looper** — 4 independent stereo layers sharing one tempo-locked loop
  length (the first layer recorded sets it for the rest). Record,
  overdub, undo, per-layer filter/character effect/reverb send, and
  project-wide vari-speed with Scrub/Freeze transport modes.
- **Dexed** — a real 6-operator, 10-voice DX7 clone built on Google's own
  **msfa** emulation core. 3129 factory presets across 54 folders, SysEx
  import for your own banks, a real algorithm diagram, and a full
  per-operator advanced editor.
- **Grains** — a monophonic granular instrument: a fixed-position grain
  cloud plus an independent sweeping scan layer, rhythmic grain
  patterns, and audio-inclusive presets. Capture live input, pull from a
  loop layer, or import a WAV.
- **Ensemble** — an 8-voice divide-down string-machine/ensemble organ in
  the ARP Solina lineage: 7 real drawbars, built-in chorus, pitch bend,
  and a mod wheel.
- **Sample Drum (SMPDR)** — a multi-pad one-shot drum sampler for Kits
  and Breaks, up to 24 slices each, with live re-slicing and direct
  import of real OP-1 drum packs.
- **Sample Sound (SMPVO)** — a melodic, chromatic sampler for one
  captured or imported sound at a time, with its own Capture/Trim/Tune
  workflow and direct import of real OP-1 instrument packs.

SMPDR and SMPVO share one underlying sample-playback engine and are
mutually exclusive — enabling one disables the other.

## Hardware

- **[Daisy Pod](https://daisy.audio/products/pod)** — two knobs, two
  buttons, one clickable encoder, two RGB LEDs, stereo in/out, all built
  in.
- **I2C OLED display** (SSD1306/SSD1309, 128x64) on the Pod's default
  I2C1 pins: `SCL`→`D11`, `SDA`→`D12`, plus `3V3`/`GND`. Default address
  `0x3C` — if it stays dark, try the commented-out `0x3D` line in
  `main.cpp`.
- **MIDI IN** (optional) — the Pod's built-in jack, plays Dexed/
  Ensemble/Grains/SampleSynth.
- **Micro SD card**, FAT32. A hand-wired socket with no card-detect pin
  — power off before swapping cards. See
  [`sdcard-template/`](sdcard-template/) for a ready-made folder set to
  copy onto a fresh card (most folders are created automatically as you
  use each feature; the Import staging folders aren't, so the template
  pre-makes those). Ships 9 starter Grains presets.
- **PZD Daisy Pod Case** — free to print on
  [CULTS3D](https://cults3d.com/en/users/Planet_Zero_Designs_PZD/3d-models)
  or [PRINTABLES](https://www.printables.com/@PlanetZeroDe_5266703), or
  buy one ready-made on
  [ETSY](https://www.etsy.com/uk/shop/PlanetZeroDesignsPZD?ref=shop_profile&listing_id=4560482983).

## Features

**Looping** — 4 tempo-synced layers; record/pause/overdub with
count-in; undo last overdub; live waveform with moving playhead.

**Per-layer** — Volume/Pan/Speed (0.3x–2x), Filter (Off/Low-pass/
High-pass/Band-pass), one character effect at a time (Drive/Bitcrush/
Chorus/Tremolo/Phaser/AutoWah/Flanger), reverb send, input gain
(1x–4x).

**Global** — Tempo (50–240 BPM)/Bars (1–16)/metronome, master filter,
one shared Dattorro-topology plate reverb (Size/Tone/Diffusion pages)
plus a shared delay, Bypass (live monitoring), and project-wide
vari-speed with Scrub/Freeze transport modes.

**Save/load** — Performances (all 4 layers' audio plus every looper/
global setting) and every engine's own presets save to SD, each with
the same Overwrite/Save New convention. Export the current performance
as a WAV — full-quality native 48kHz, or 44.1kHz straight into a
`custom/` folder so the same card drops into MicroDexed Touch's second
slot with no copying needed.

**Mixer** (`Global:Mixer`) — one screen with Volume/Pan/Reverb-send for
all 4 layers plus every engine and Bypass, a live oscilloscope of the
final mix, per-channel peak meters with clip indicators, and a live CPU
meter.

**SD card management** (`Global:SdMgmt`) — a grid of all 8 save
categories (Performances, Grains/Dexed/Ensemble presets, and
SampleSynth's Captures/Kits/Voice User/Drum User presets) — duplicate
or delete any file directly on the device, no computer needed.

**Sample library on the card** — SampleSynth content lives in numbered
folders of up to 32 items under `SAMPLES/`: `SOUNDS1`, `CAPTURED1`,
`KITS1`, `BREAKS1`, … When one fills up, the next is created
automatically, so the library grows with the card. Each Kit or Break has
its own folder holding its pad sounds. The Preset page browses folder
first, then item.

### Importing real OP-1 sample packs

The actual licensed OP-1 packs aren't bundled with this firmware, but
you can download your own from [op1fun](https://op1.fun/) (or export one
from the OP-1 pack builder) as `.aif` files and convert them yourself:

1. Drop `.aif`/`.aiff` files into the staging folders at the card's
   root:
   - `DRUM PACK IMPORT/` — drum packs. Each becomes a **Kit**, or a
     **Break** if its typical slice is a second or longer.
   - `BREAK PACK IMPORT/` — always imported as Breaks.
   - `SOUND PACK IMPORT/` — instrument packs, each becoming a **Sound**.
2. On the Pod: `Global:SMPDR` or `Global:SMPVO` → **Import** page.
   Button 1 cycles the target (Drums/Sounds/All); hold Button 2 to
   import.
3. One press imports every file, however many. Splice points and pitch
   come straight from the pack's own metadata — no manual trimming.
4. Imported files move into `Done/`. A file that fails is retried, then
   left in place for the next press.

**Recording Dexed, Ensemble, Grains, or SampleSynth into a loop layer**
— playing any instrument while a layer is recording/overdubbing captures
its live sound directly into the take, mixed with your physical input,
no patch cable needed.

### CPU tips

CPU headroom isn't a real concern on the current build, but a few
habits keep it on your side:

- Looper, Dexed, Ensemble, Grains, and SampleSynth are each
  independently switchable from their own `Global:` page — switching
  one off frees its CPU share entirely, not just its output.
- SMPVO/SMPDR share one engine and are mutually exclusive, so only one
  of them ever costs CPU at a time.
- Record your synth parts into loop layers first, then switch those
  engines off once you've got the take you want.
- Dial in reverb/delay last, once you know what's actually being kept —
  several engines sending into effects at once is the heaviest realistic
  combination.
- `Global:Mixer`'s own footer shows a live CPU meter if you want to
  check headroom directly.

## How to use it

**Recording your first loop**
1. On the Home screen, rotate the encoder to pick a layer (1–4).
2. Hold Button 1 to arm recording — the metronome counts in a bar first
   if it's on.
3. Play. Recording stops automatically at the bar loop length (or tap
   Button 1 to stop early).
4. Tap Button 1 to pause/resume playback; hold it again to overdub more
   onto the same layer.

**Navigating the menu**
- **Rotate** the encoder: on Home, moves between layers; on any other
  page, cycles through that page's sub-pages.
- **Click** the encoder: from Home, opens the selected layer's pages; on
  any Global page hosting an instrument (Dexed/Ensemble/Grains/SMPVO/
  SMPDR) or the Mixer, drills into that screen; anywhere else, mutes/
  unmutes all 4 loop layers.
- **Long-press** the encoder: from Home, opens Global settings; from
  anywhere else, goes back to Home.

Both knobs and both buttons are "soft" — what they do depends on which
page is open, and it's always shown on screen in the footer.

**Knob pickup** — knobs don't jump the moment you touch them. Each one
shows its live current value; turning the physical knob only takes
control once it reaches that value.

**Basic workflow**, using pocket operators (applied effects before
making the video). Also includes a WAV file of acoustic guitar input,
MicroDexed Touch for drums, and testing using some headphones as a mic
input (put the gain up to x4):
https://github.com/user-attachments/assets/1c081390-e61f-4a1b-9088-a552860b9a5d
[Headphones_as_mic.WAV](https://github.com/user-attachments/files/31609801/Headphones_as_mic.WAV)

**4 layers with Dexed, Grains, PO-12 rhythm, and live mic input** — a
full performance combining everything above at once:
https://github.com/user-attachments/assets/025a71a1-f06f-424a-bb34-b3bdb560e121

**LEDs** — LED1 shows the looper's transport at a glance: green = any
layer playing, blue = any layer paused, off = looper disabled or nothing
playing/paused yet. LED2 lights red whenever something is actively
recording — a loop layer recording/overdubbing, or a Grains/SampleSynth
capture in progress.

## Control reference

| Page | Knob 1 | Knob 2 | Button 1 | Button 2 |
|---|---|---|---|---|
| Home | Master volume | Metronome volume | Rec / Pause / Overdub (tap/hold, state-dependent) | Toggle bypass |
| Layer: Status | Volume | Pan | Same transport as Home | Tap = Undo last overdub, hold 800ms = Clear layer |
| Layer: Gain | Input gain | — | — | — |
| Layer: Speed | Speed | — | Tap = reset to 1.0x | — |
| Layer: Filter | Cutoff | Resonance | Cycle filter mode | — |
| Layer: Effect | Effect param A | Effect param B (Drive: output trim, center = unchanged) | Cycle effect | — |
| Layer: LYR-FX | Send | — | — | — |
| Global: Tempo | BPM (50–240) | Bars (1–16, lower-capped at slower BPM) | Toggle metronome | Hold 800ms = Save as startup default |
| Global: Speed | Vari-speed | — | Cycle Normal/Scrub/Freeze | Reset to 1.0x |
| Global: Filter | Cutoff | Resonance | Cycle filter mode | — |
| Global: GLB-FX | Size (= Decay) | Bypass reverb send | — | — |
| Global: RevTone | PreDelay / DecayDiffusion | PreFilter / Damping | Knobs → PreDelay+PreFilter | Knobs → DecayDiffusion+Damping |
| Global: RevDiff | Input Diffusion 1 | Input Diffusion 2 | — | — |
| Global: File | Choosing Save: Overwrite/Save New; Browsing Load: scroll files | — | Save / Select file list (Load) / Back | Hold 800ms = Save or Load, whichever's open |
| Global: SD Mgmt | Scroll (category grid, folder list, or file list) | — | Tap = drill in/back, Hold 400ms = Duplicate | Tap = open folder (Captures/Kits), Hold 1500ms = Delete |
| Global: Export | — | — | Tap = Export, native 48kHz ("Studio") | Tap = Export, 44.1kHz for MicroDexed ("CD") |
| Global: Dexed | — | — | Toggle Dexed on/off (off by default) | — (push encoder to enter Screen::Dexed) |
| Global: Ensemble | — | — | Toggle Ensemble on/off (off by default) | — (push encoder to enter Screen::Ensemble) |
| Global: Granular | — | — | Toggle Grains on/off | — (push encoder to enter Screen::Granular) |
| Global: SMPVO | — | — | Toggle SMPVO on/off (disables SMPDR) | — (push encoder to enter Screen::SampleSynth) |
| Global: SMPDR | — | — | Toggle SMPDR on/off (disables SMPVO) | — (push encoder to enter Screen::SampleSynth) |
| Global: Looper | — | — | Toggle the whole 4-layer looper on/off | — |
| Global: Mixer | — | — | — | — (push encoder to enter the Mixer screen) |

**Dexed (`Screen::Dexed`, rotate to cycle pages)**

| Page | Knob 1 | Knob 2 | Button 1 | Button 2 |
|---|---|---|---|---|
| Algo | Cycle algorithm (all 32, quantized) | — | Cycle back | Cycle forward |
| Feedback | Feedback amount (0-7) | — | — | — |
| Vibrato | LFO speed | LFO pitch-mod depth | — | — |
| Brightness | Modulator level scale (center = as saved) | — | — | — |
| EnvSpeed | Envelope rate scale (center = as saved) | — | — | — |
| Filter | Cutoff | Resonance | Cycle filter mode | — |
| Mix | Reverb send | Output level | — | — |
| DX-FX | Reverb send / Delay send | Shared reverb size / delay time | Toggle Reverb/Delay | — |
| DX-FOLD | Fold Amount (0% = bypass) | Output trim | — | — |
| MIDI | MIDI channel (Omni, 1-16) | — | — | — |
| Advanced | — | — | — | — (push encoder to enter the operator editor) |
| Preset | Top: Files/Import/New. Files → Roms/Dexed/Imports/User group → folder → presets | — | Save / Select (top) / Back (group+folder) | Open (group+folder) / Hold: Import / Tap: preview, Hold: confirm |

The Algo page shows a real box-and-arrow diagram of whichever algorithm
is loaded (filled boxes = carriers, outlined = modulators, "FB" tags the
feedback operator). Clicking the encoder on any Dexed page other than
Advanced mutes/unmutes all 4 loop layers. Pan isn't on this screen — see
Mixer below.

**Dexed advanced editor (`Screen::DexedOperator`, entered from the
Advanced page above, rotate to cycle pages)**

| Page | Knob 1 | Knob 2 | Button 1 | Button 2 |
|---|---|---|---|---|
| Algo | — (read-only; edit from Global:Dexed's own Algo page) | — | Cycle operator (1-6) | — |
| Ratio | Coarse ratio (0-31) | Output level | Cycle operator (1-6) | — |
| Detune | Detune (0-14, 7=centered) | — | Cycle operator (1-6) | — |
| EG Rate | Attack | Decay (or Sustain/Release) | Cycle operator (1-6) | Toggle AD/SR |
| EG Level | Attack Level | Decay Level (or Sustain/Release Level) | Cycle operator (1-6) | Toggle AD/SR |
| AmpModSens | Mod wheel Amp/EG Bias sensitivity (0-3) | — | Cycle operator (1-6) | — |
| VelocitySens | Key velocity sensitivity (0-7) | — | Cycle operator (1-6) | — |
| KeybRateScale | Keyboard rate scaling (0-7) | — | Cycle operator (1-6) | — |

A short encoder click here returns to Screen::Dexed's own Advanced page;
long-press goes all the way to Home.

**Ensemble (`Screen::Ensemble`, rotate to cycle pages)**

| Page | Knob 1 | Knob 2 | Button 1 | Button 2 |
|---|---|---|---|---|
| Registration | Select drawbar (0-6) | Level of the selected drawbar | — | — |
| Env | Attack / Sustain | Decay / Release | — | Toggle AD/SR |
| Ensemble (Chorus) | Chorus depth | Chorus rate | — | — |
| Filter | Cutoff | Resonance | Cycle filter mode | — |
| Mix | Output level | Pan | — | — |
| FX | Reverb send / Delay send | Shared reverb size / delay time | — | Toggle Reverb/Delay |
| MIDI | MIDI channel (Omni, 1-16) | — | Cycle mod wheel target (Off/Chorus/Level/Filter) | — |
| Preset | Top: Files/New. Files → combined factory+user list | — | Save / Back | Select (top) / Tap: preview, Hold: confirm |

Clicking the encoder on any Ensemble page mutes/unmutes all 4 loop
layers. Boots with all 7 drawbars flat at 60% before any preset is
loaded.

**Grains (`Screen::Granular`, rotate to cycle pages)**

| Page | Knob 1 | Knob 2 | Button 1 | Button 2 |
|---|---|---|---|---|
| Grain | Size / Grain's own Gap | Fill / Jitter | Knobs → Size+Fill | Knobs → Gap+Jitter |
| Position | Position | Direction (Grain's own) | Cycle Rhythm pattern | Cycle Speed |
| Scan | Scan speed/direction | Scan's own Fill | Toggle Bounce/Wrap | Cycle Scan's own Direction |
| ScanRange | Scan Start / Scan's own Gap | Scan End | Knobs → Start+End | Knob → Gap |
| Tune | Tune | — | Toggle Map-to-Note | — |
| ADSR | Attack / Sustain | Decay / Release | Knobs → Attack+Decay | Knobs → Sustain+Release |
| Filter | Cutoff | Resonance | Cycle filter mode | — |
| Mix | Grain volume / Reverb send | Scan volume | Knobs → Grain+Scan | Knobs → Reverb send |
| GR-FX | Reverb send / Delay send | Shared reverb size / delay time | Toggle Reverb/Delay | — |
| MIDI | MIDI channel (Omni, 1-16) | — | — | — |
| Capture | — | — | Cycle source (Direct/Layer/Import) | Hold = record or capture |
| Trim | Trim start | Trim end | Play trimmed section | — |
| Preset | See Save/load above | — | Save / Select (Load) / Back | Hold 800ms = confirm |

Clicking the encoder on any Grains page mutes/unmutes all 4 loop layers.

**SampleSynth (`Screen::SampleSynth`, shared by SMPVO/SMPDR — rotate to
cycle pages)**

Shared by both:

| Page | Knob 1 | Knob 2 | Button 1 | Button 2 |
|---|---|---|---|---|
| Filter | Cutoff | Resonance | Cycle filter mode | — |
| Env | Attack / Sustain | Decay / Release | Target AD | Target SR |
| Mix | Output level | Pan | — | — |
| FX | Reverb send / Delay send | Shared reverb size / delay time | Target Reverb | Target Delay |
| MIDI | MIDI channel (Omni, 1-16) | — | — | — |
| Import | — | — | Cycle target (Drums/Sounds/All) | Hold 800ms = Import |
| Preset | Folder (SMPVO: Sounds n/Captured n, SMPDR: Kits n/Breaks n) → item | — | — (Back while browsing) | Select/Open, Hold 800ms = Load |
| User | — | — | Save (Overwrite/New, Idle) / Back | Select/Open, Hold 800ms = Load/Save |

SMPVO-only:

| Page | Knob 1 | Knob 2 | Button 1 | Button 2 |
|---|---|---|---|---|
| Capture | — | — | Cycle source (Direct/Layer/Import) | Hold = record or capture |
| Trim | Trim start | Trim end | Play trimmed section | Hold 800ms = save as Sound |
| Tune | Cap Tune (±24 semitones) | Imp Tune (±24 semitones) | Mono/Poly-Cap | Mono/Poly-Imp |

SMPDR-only:

| Page | Knob 1 | Knob 2 | Button 1 | Button 2 |
|---|---|---|---|---|
| Setup | Tune (±24 semitones, live) | Slices (0-16, pending) | Toggle Mono/Choke (per-kit) | Hold 800ms = apply Slices |

Clicking the encoder on any SampleSynth page mutes/unmutes all 4 loop
layers. See **Importing real OP-1 sample packs** above for how Kits/
Sounds get onto the card.

**Mixer (`Screen::Mixer`, rotate to step through channels)**

Each of the 10 channels (Layer 1–4, Grains, Dexed, Ensemble, SampleSynth,
Bypass, Master) gets its own stop; rotating past the last one shows a
live oscilloscope of the final output mix, then wraps back to Layer 1.

| Channel | Knob 1 | Knob 2 | Button 1 | Button 2 |
|---|---|---|---|---|
| Layer 1–4 / Grains / Dexed / Ensemble / SampleSynth / Bypass | Volume | Pan | Knobs → Volume+Pan | Knobs → Reverb send |
| Master | Volume | Reverb size | — | — |

## Building and flashing

As of v1.6.0 this firmware boots via a small separate bootloader and runs
from the Pod's external QSPI flash chip instead of internal flash, which
was nearly full — see [DESIGN.md](DESIGN.md) for why. This changes how
flashing works, and means a **one-time setup step** on any Pod that
hasn't run this bootloader before.

**One-time setup** (skip this if your Pod already has the bootloader —
e.g. you've flashed v1.6.0 or later before):

1. Put the Pod into DFU mode (hold the `BOOT` button, tap `RESET`, release
   `BOOT`).
2. Flash the bootloader itself — either:
   - `dsy_bootloader_v6_4-intdfu-2000ms.bin`, attached to this project's
     [Releases](../../releases), via
     [Electrosmith's web programmer](https://flash.daisy.audio/) (no
     toolchain needed), or
   - from source: `cd src && make program-boot`

From then on, the Pod boots straight into whatever app is on QSPI —
normal power-on, no button-holding, just a slightly longer boot.

**Flashing the looper firmware itself** (every time, including that very
first time right after the bootloader install above):

- **Easiest — SD card**: copy `main.bin` (from
  [Releases](../../releases), or your own `src/build/main.bin`) onto the
  root of a FAT32 SD card, insert it into the Pod, and power-cycle. The
  bootloader notices any `.bin` file on the card and flashes it
  automatically — no DFU window to catch, works every time. If it's a
  fresh card, copy the contents of [`sdcard-template/`](sdcard-template/)
  onto the same root first (or alongside `main.bin`, order doesn't
  matter).
- **Alternative — DFU**: put the Pod into DFU mode the same way as above,
  then either `make program-dfu` from `src/`, or drop `main.bin` onto
  [Electrosmith's web programmer](https://flash.daisy.audio/). The
  bootloader's own DFU window is brief (~2 seconds) once it boots, so the
  SD card method above is generally more forgiving.

**Building from source**:

Needs the [ARM GNU toolchain](https://developer.arm.com/downloads/-/arm-gnu-toolchain-downloads)
(`arm-none-eabi-gcc`) and `make`. The two libraries are built once, then
the firmware:

```
git clone --recurse-submodules https://github.com/Peteranthony1st/PZD-Daisy-Pod-Groovebox.git
cd PZD-Daisy-Pod-Groovebox/src
make -C libDaisy
make -C DaisySP && make -C DaisySP/DaisySP-LGPL
make
```

`build/main.bin` is what you flash via either method above. After pulling
changes that move `libDaisy` or `DaisySP` to a new commit, run
`git submodule update` and rebuild that library (`make -C libDaisy clean
&& make -C libDaisy`, likewise for DaisySP) before `make`.

## Thanks

- [**kooliha's Ouroboros Loop Station**](https://github.com/kooliha/Ouroboros_Loop_Station)
  — this project's original concept and file organization came from
  that design, a DIY 5-track stereo loop station for the Daisy Seed
  (the name has since diverged, grown well past a pure loop station).
  This firmware follows its per-layer loop-buffer struct as a direct
  structural template, then rewrites everything else — no direct
  hardware coupling in the DSP layer, plus filter/effects/reverb,
  the OLED menu, and SD save/load, none of which exist in the original.
  See [DESIGN.md](DESIGN.md) for the specifics.
- **[Robey Pointer](http://robey.lag.net/2010/01/23/tiny-monospace-font.html)**,
  for the tiny "Tom Thumb" font used throughout the OLED UI — a
  readability-tuned version of the original 3x5 font by Brian J.
  Swetland and Vassilii Khachaturov, ported here from Adafruit-GFX.
- **Google's [msfa](https://github.com/google/music-synthesizer-for-android)**
  (Apache License 2.0) — the real DX7 emulation core Dexed is built
  around, vendored unmodified into `src/msfa/`.
- The broader **Dexed/MicroDexed** open-source ecosystem — source of
  Dexed's 3129 factory presets, freely distributed real DX7 SysEx
  bank dumps.
- **[el-visio's dattorro-verb](https://github.com/el-visio/dattorro-verb)**
  (MIT License, Pauli Pölkki 2022) — a real C implementation of Jon
  Dattorro's 1997 figure-8 plate reverb topology, ported here (`src/
  dattorro_reverb.h/.cpp`) as the shared reverb bus — same real delay
  lengths, tap offsets, and tuned defaults, just `float`/static-buffer
  instead of `double`/`malloc`.
- **[op1fun](https://op1.fun/)** and the broader OP-1 community — not
  code vendored into this firmware, but the real community resource
  SampleSynth's OP-1 pack Import feature is built to read: real `.aif`
  drum/sampler packs, complete with the splice/pitch metadata this
  firmware decodes directly rather than guessing at. Nothing from
  op1fun is bundled or redistributed here — the feature exists so you
  can bring your own packs in, not to ship anyone else's content.
