SampleSynth Voice (SMPVO) Multi-samples -- one folder per instrument,
holding one .wav per sampled note. Each note plays from its nearest
recording instead of one sample stretched across the whole keyboard.

The note comes from each file's name: put it last, after a space, _ or -
("Cello C3.wav", "Cello_C3.wav", "Pad-F#2.wav"), or use Salamander-style
names ("A0v10.wav", "C4vH.wav"). Files with no note in the name are
skipped. Each note covers the keys halfway to its neighbours; the lowest
and highest reach the ends of the keyboard.

Limits: 24 notes, and about 33 s of audio per instrument in total (every
file counts, mono or stereo). Trim long tails with a short fade -- the
release envelope recreates them. Any sample rate works.

Make your own: create a folder in any MULTISMPn folder and copy the WAVs
in. Up to 32 instruments per MULTISMPn folder.

Load from Screen::SampleSynth (Voice) -> Preset page -> Multi -> Multi 1.
Delete/Duplicate from Global:SdMgmt -> SMPIMP -> Voice -> Multi.

"Upright Piano" is included as a ready-made example (CC0 public domain,
see its own README.txt).
