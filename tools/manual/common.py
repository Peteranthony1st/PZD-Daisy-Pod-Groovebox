# Paths shared by the manual's scripts (see build.sh).
import os
HERE       = os.path.dirname(os.path.abspath(__file__))
REPO       = os.path.normpath(os.path.join(HERE, '..', '..'))
SCREENS_PY = os.path.join(REPO, 'tools', 'readme_screens.py')
LOGO       = os.path.join(HERE, 'assets', 'pzd-logo-white.png')
# The PZD case's STL files (TOP, BASE, KNOB) -- not in the repo.
CASE_DIR   = os.environ.get('PZD_CASE_DIR', os.path.expanduser('~/Documents/PZD-GROOVE-CASE'))
