# Quick Start guide and User Manual

`build.sh` draws the Quick Start guide (`docs/quick-start.pdf`, and its pages
as JPEGs in `docs/quick-start-jpg/` for posting), the README's
gallery of the Groovebox (`images/pod-*.png`) and the User Manual
(`docs/user-manual.pdf`).

## The User Manual

The chapters are HTML in `manual/chapters/`, laid out by `manual/manual.css`
and printed to PDF by Chrome (`build_manual.py`). Every screen in it comes
from the **host renderer** (`host/`): the firmware's own `ui.cpp` and engines,
built for the computer with stand-ins for the Pod's hardware (`host/fake/`:
knobs, buttons and encoder, a 128x64 screen drawn by libDaisy's own drawing
code, and an SD card served from a folder). A screen is a script of what a
player does, written in the chapter where the picture goes:

    <pre class="shot" id="layer-filter">wait 2000
    click
    turn 2</pre>

It starts from power-on with the card template as the card. `@demo` loads
the demo song first, `@engine N` switches on and opens the engine on Global
page N, `@airy` and `@808` load a starter sound (see `build_manual.py`).
The renderer runs no audio, so a screen that needs the beat moving (a track
recording or playing) can't be captured yet.

## The Quick Start

- **The Groovebox drawings** come from the PZD case's own STL files (TOP,
  BASE and KNOB), rendered from above and from each side, so every hole,
  engraving and knob is where the printed case has it. The STLs aren't in
  the repo: set `PZD_CASE_DIR` if they aren't in `~/Documents/PZD-GROOVE-CASE`.
- **Every screen** is drawn pixel for pixel by `tools/readme_screens.py`'s
  code, which copies `ui.cpp`'s drawing. A screen that's only in the guide
  (Global:Perf's save, SSaw:Preset) is copied from `ui.cpp` the same way, in
  the page script that shows it. Change the firmware's drawing, change these.

| Script | Makes |
|---|---|
| `stl.py` | STL loading and an orthographic depth render |
| `masks.py`, `knob.py`, `sides16.py`, `sidestyle.py` | the case's top, knob and side views |
| `draw.py` | the Groovebox, in colour or line art, with a screen on it |
| `mock2.py`, `sidesfig.py` | the labelled diagrams |
| `page.py` | the page styles, page 2 and the manual's sample page |
| `page2.py`, `page5.py`, `page7.py`, `cover.py` | pages 3–8 and the cover |
| `readme_photos.py` | the README's gallery, `images/pod-*.png` |

Needs Python 3 with numpy, scipy and pillow, macOS's Avenir Next, a C++ compiler
(for the renderer) and Google Chrome (for the manual's PDF).
