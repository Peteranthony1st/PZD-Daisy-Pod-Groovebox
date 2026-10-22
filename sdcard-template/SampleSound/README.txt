Drop raw OP-1 sampler-pack .aif files here (downloaded from op1fun, or
exported from the OP-1 pack builder) -- one .aif per sound.

Screen::SampleSynth -> Import page: Button1 picks Kits/Sounds/All,
Button2 held converts every .aif in here into a real, playable Sound
(Screen::SampleSynth -> Preset page -> Sounds), tuned to the pack's own
real pitch automatically. A plain recording with no OP-1 pitch data
still imports fine, at a sensible default pitch.

Successfully-imported files move into their own Done/ subfolder
automatically, so running Import again never reprocesses them.
