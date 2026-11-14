# Quick Start guide

`build.sh` draws the Quick Start guide and writes `docs/quick-start.pdf`.

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

Needs Python 3 with numpy, scipy and pillow, and macOS's Avenir Next.
