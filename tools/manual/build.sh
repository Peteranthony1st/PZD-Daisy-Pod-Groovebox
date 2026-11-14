#!/bin/sh
# Builds the Quick Start guide: every drawing from the PZD case's STL files
# and every screen from the firmware's own drawing code (via
# tools/readme_screens.py), then docs/quick-start.pdf.
#
#   tools/manual/build.sh
#
# Needs Python 3 with numpy, scipy and pillow (PYTHON=... to pick one), the
# case STLs in ~/Documents/PZD-GROOVE-CASE (or PZD_CASE_DIR=...), and
# macOS's Avenir Next font.
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
PY=${PYTHON:-python3}
mkdir -p "$HERE/build" && cd "$HERE/build"
export PYTHONPATH="$HERE"
for s in masks knob sides16 mock2 sidesfig page page2 page5 page7 cover readme_photos; do
    echo "  $s"
    "$PY" "$HERE/$s.py" > /dev/null
done
"$PY" - "$HERE/../../docs/quick-start.pdf" <<'PY'
import sys
from PIL import Image
pages=['page-qs-1','page-quickstart']+['page-qs-%d'%n for n in range(3,9)]
ims=[Image.open(p+'.png').convert('RGB') for p in pages]
ims[0].save(sys.argv[1],save_all=True,append_images=ims[1:],resolution=150)
print('  docs/quick-start.pdf, %d pages'%len(ims))
PY
