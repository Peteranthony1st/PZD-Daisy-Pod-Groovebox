SampleSynth Drum (SMPDR) Kits -- one folder per kit, holding its pad
sounds and (usually) a KIT.DAT with the pad mapping and settings. Up to
24 pads and about 33 s of audio per kit.

Two ways to make one:
- Import an OP-1 drum pack from DRUM PACK IMPORT/KITS/: each pack becomes
  a folder named after the file.
- Make a folder here yourself and copy WAVs into it. With no KIT.DAT, the
  WAVs load in name order, one per key from C2 up ("01 kick.wav",
  "02 snare.wav" sets the order).

Kits play poly (pads ring over each other); turn Choke on per kit on the
Setup page. Up to 32 kits per KITSn folder; KITS2/, KITS3/, ... are
created automatically.

Browse from Screen::SampleSynth (Drum) -> Preset page: pick a folder
("Kits 1"), then a kit. Delete/Duplicate from Global:SdMgmt -> SMPIMP ->
Drum -> Kits. Deleting a kit removes its whole folder.
