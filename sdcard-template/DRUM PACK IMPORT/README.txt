Drop OP-1 drum-pack .aif (or .aiff) files here (downloaded from op1fun,
or exported from the OP-1 pack builder) -- one file per kit. Plain
recordings with no OP-1 data import too, as a single pad.

Screen::SampleSynth -> Import page: Button1 picks Drums/Sounds/All,
Button2 held converts every file in here into a playable Kit or Break
(Screen::SampleSynth -> Preset page), each in its own folder under
SAMPLES/KITSn/ or SAMPLES/BREAKSn/. Each slice lands on its own key,
using the splice points already baked into the file.

Kit or Break is decided automatically: if a pack's typical slice is a
second or longer it's filed as a Break, otherwise as a Kit. To force a
Break, use BREAK PACK IMPORT/ instead.

However many files are in here, one Import processes them all.
Successfully-imported files move into their own Done/ subfolder. A file
that fails is tried twice, then left here for the next Import.
