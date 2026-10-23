Drop .wav files here to import them into Grains
(Screen::Granular -> Capture page -> cycle source to Import) or into
SampleSynth (Screen::SampleSynth -> Capture page -> cycle source to
Import -> Trim/Tune -> hold Button 2 to save as a Sound).

This is also where a plain multi-sample pack goes -- one that isn't a
real OP-1 .aif export with splice/pitch metadata, just a folder of
individual one-shot .wav files (e.g. Teenage Engineering's own official
sound packs like "Snaps"). Drop the whole folder's .wav files in here,
then import them into SampleSynth one at a time: Capture -> Import ->
Knob 1 picks which file -> hold Button 2 to load it -> Trim/Tune/save.
There's no batch version of this yet -- see DRUM PACK IMPORT/SOUND
PACK IMPORT's own README.txt for the one pack format that does
batch-convert automatically, straight to a multi-pad Kit.

Accepted: 16/24/32-bit PCM or 32-bit IEEE float WAV, mono or stereo, at
any of these sample rates -- anything else is resampled automatically:
8000, 11025, 16000, 22050, 24000, 32000, 44100, 48000, 88200, 96000,
176400, 192000 Hz.

This README.txt is harmless to leave here -- the firmware only ever
looks for real .wav/.WAV files in this folder.
