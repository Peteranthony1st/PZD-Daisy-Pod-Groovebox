Drop OP-1 sampler-pack .aif (or .aiff) files here (downloaded from
op1fun, or exported from the OP-1 pack builder) -- one file per sound.

Screen::SampleSynth -> Import page: Button1 picks Drums/Sounds/All,
Button2 held converts every file in here into a playable Sound
(Screen::SampleSynth -> Preset page -> Sounds n), tuned to the pack's
own real pitch automatically. A plain recording with no OP-1 pitch data
still imports fine, at a sensible default pitch. Sounds fill
SAMPLES/SOUNDS1/ first, then SOUNDS2/, ... (32 per folder).

However many files are in here, one Import processes them all.
Successfully-imported files move into their own Done/ subfolder. A file
that fails is tried twice, then left here for the next Import.
