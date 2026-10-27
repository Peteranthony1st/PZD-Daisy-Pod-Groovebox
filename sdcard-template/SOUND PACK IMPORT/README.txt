Drop OP-1 sampler-pack .aif (or .aiff) files here (from op1.fun, or the
OP-1 pack builder) -- one file per sound. Only "sampler" patches import:
OP-1 synth presets (e.g. "cluster", "string") hold engine settings and a
short preview, not a sample, and are skipped.

SMPVO -> Import page: K1 picks where they go (a new Sounds bank, or an
existing one with room for all of them), hold Button2 to import. Each
becomes a Sound tuned to the pack's own pitch (Screen::SampleSynth ->
Preset page -> Sounds). Plain .wav files don't need importing -- copy them
straight into a SAMPLES/SOUNDSn folder.

However many files are in here, one Import processes them all.
Imported files move into Done/ (a name already there is kept as
"name (2)"). A file that fails is tried twice, then left here.
