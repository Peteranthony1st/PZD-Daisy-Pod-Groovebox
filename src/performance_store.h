#pragma once
#include <cstddef>
#include <cstdint>
#include "tempo_clock.h"
#include "looper_layer.h"
#include "granular_engine.h"
#include "dexed_synth.h"
#include "ensemble_synth.h"
#include "sample_synth.h"
#include "shared_delay.h"

// SD card save/load for a whole "performance" (all layers' audio plus
// tempo/global/per-layer settings) as one flat binary file per slot.
//
// Slots are numbered 1..kMaxSlots and map to fixed 8.3 filenames
// ("PERF001.DAT" etc.) so listing/parsing never depends on long-filename
// support -- the UI is free to show a friendlier label ("1 - Performance")
// without that label ever touching the filesystem.
//
// This module owns the SD card hardware (SdmmcHandler + FatFSInterface)
// itself -- LooperLayer stays hardware-free (see its header comment) by
// only exposing plain getters/setters and raw buffer access; all file I/O
// lives here instead.
//
// Save()/Load() are blocking (SD I/O, one loop buffer can be many
// megabytes) -- call them from main()'s loop, never from the audio
// callback, same rule as Ui::Update()'s OLED writes. Both take an optional
// progress callback so the caller can redraw a progress bar during the
// (up to a couple of seconds) transfer.
namespace PerformanceStore
{
constexpr int kMaxSlots = 99;

// Mount the SD card. Call once from main(), after hw.Init().
void Init();
bool IsCardPresent();

// Re-mount the card (same f_mount() call Init() does). This hand-wired
// SD socket has no card-detect pin, so the firmware has no way to know a
// card was physically pulled/swapped/reinserted -- IsCardPresent()
// otherwise stays whatever it was at boot forever. Call this whenever
// it's reasonable to assume the card might have changed (see
// Ui::HandleEncoder()'s Home->Global transition) rather than waiting for
// a Save/Load/Export call to fail against a stale mount.
void Remount();

// Short diagnostic for the most recent Save()/Load() failure -- a step
// tag plus the FatFS FRESULT code ("hdr:3", "aud0:5", ...), e.g. for
// display on the File page. Empty string if the last call succeeded (or
// nothing has run yet). Overwritten by every Save()/Load() call.
const char* GetLastError();

// Fills out_slots ascending with existing slot numbers, returns how many
// were found (up to max_out).
int ListSlots(int* out_slots, int max_out);
// Lowest slot number with no saved file yet, or -1 if every slot is full
// or no card is present.
int NextFreeSlot();

using ProgressFn = void (*)(float progress01);

// The shared delay's own settings plus Bypass's Delay Send -- saved in
// performances and startup defaults from file version 16 on. Loading an
// older file leaves the live values untouched (those files never stored
// them), so an old performance sounds exactly as it did before.
struct DelayState
{
    SharedDelay::Settings settings;
    float                 to_reverb01   = 0.f; // Global:Dly-Rev
    float                 bypass_send01 = 0.f; // Global:Bypass, Delay Send
};

// Deletes an existing saved performance -- irreversible, same as deleting
// any other file; the caller (SD MGMT's own hold-to-confirm gesture) owns
// making that consequence clear before calling this. Also closes the gap
// this leaves behind: every higher-numbered performance still on the card
// (plus any other pre-existing gaps above it) shifts down to keep the
// numbering contiguous from 1, so a later Save New always continues right
// after the highest real performance instead of NextFreeSlot() picking
// the low number this just freed. If loaded_slot_inout is non-null and
// currently names a slot this shifts (or the one just deleted), it's
// updated in place to follow that content to its new slot (or to -1 if it
// was the slot just deleted).
bool DeleteSlot(int slot, int* loaded_slot_inout = nullptr);
// Byte-for-byte copy of an existing performance into the next free slot
// (see NextFreeSlot()) -- chunked, so this can take real time for a
// performance with a lot of recorded audio; pass on_progress the same way
// Save()/Load() do. *out_new_slot is set to the slot actually used.
bool DuplicateSlot(int slot, int* out_new_slot, ProgressFn on_progress = nullptr);

// Deals ONLY with the looper (all layers' audio plus tempo/global/per-
// layer settings) -- Grains has its own fully independent save/load
// system (SaveGranularPreset() below), never embedded into a performance
// file. Keeps every instrument's saves in its own separate "bubble",
// performances included.
bool Save(int                slot,
          TempoClock&        tempo,
          LooperLayer*       layers,
          int                num_layers,
          float              master_volume01,
          bool               bypass,
          FilterMode         master_filter_mode,
          float              master_filter_cutoff01,
          float              master_filter_res01,
          float              reverb_size01,
          float              bypass_reverb_send01,
          // The other 6 DattorroReverb parameters (see Ui::GetReverbPreDelay01()
          // and friends) -- reverb_size01 above IS Decay, already covered.
          float              reverb_predelay01,
          float              reverb_prefilter01,
          float              reverb_input_diffusion1_01,
          float              reverb_input_diffusion2_01,
          float              reverb_decay_diffusion01,
          float              reverb_damping01,
          // Which MIDI channel each engine listens on -- -1 = Omni. See
          // Ui::GetDexedMidiChannel()/GetGranularMidiChannel()/
          // GetEnsembleMidiChannel()/GetSampleSynthMidiChannel().
          int                dexed_midi_channel,
          int                granular_midi_channel,
          int                ensemble_midi_channel,
          int                sample_synth_midi_channel,
          const DelayState& delay,
          ProgressFn         on_progress = nullptr);

// Accepts kFileVersion (15) and the four previous versions (14, 13, 12,
// 11) -- see Load()'s own comment in performance_store.cpp for the
// incremental per-version chunk scheme this uses to stay backward-
// compatible. A v11 file leaves *out_reverb_predelay01 and friends at
// the Dattorro library's own tuned defaults (same ones
// DattorroReverb::Init() uses); a v11 or v12 file leaves
// *out_dexed_midi_channel/*out_granular_midi_channel at Omni (-1); a
// v11, v12, or v13 file leaves *out_ensemble_midi_channel at Omni (-1);
// a v11-v14 file leaves *out_sample_synth_midi_channel at Omni (-1) too
// -- none of these fail to load.
bool Load(int          slot,
          TempoClock&  tempo,
          LooperLayer* layers,
          int          num_layers,
          float*       out_master_volume01,
          bool*        out_bypass,
          FilterMode*  out_master_filter_mode,
          float*       out_master_filter_cutoff01,
          float*       out_master_filter_res01,
          float*       out_reverb_size01,
          float*       out_bypass_reverb_send01,
          float*       out_reverb_predelay01,
          float*       out_reverb_prefilter01,
          float*       out_reverb_input_diffusion1_01,
          float*       out_reverb_input_diffusion2_01,
          float*       out_reverb_decay_diffusion01,
          float*       out_reverb_damping01,
          int*         out_dexed_midi_channel,
          int*         out_granular_midi_channel,
          int*         out_ensemble_midi_channel,
          int*         out_sample_synth_midi_channel,
          DelayState*  inout_delay,
          ProgressFn   on_progress = nullptr);

// Grains (GranularEngine) presets: parameters AND the captured audio
// itself -- a Grains preset IS a specific captured sound plus how it's
// being played back, so loading one needs to restore both. No factory
// range (there's no hand-tuned-patch equivalent for a captured sample),
// so every slot 1..kMaxGranularPresets is a normal user save. Numbered/
// found the same way as ListSlots()'s own range.
constexpr int kMaxGranularPresets = 99;

// audio_l/audio_r: whatever GranularEngine::GetSourceL()/R() currently
// point at; audio_len: GetSourceLen(). Blocking (chunked SD write, same
// kChunkSamples streaming as Save()'s own layer audio) -- call from the
// main loop with a progress callback, same rule as Save()/Load()/
// ExportWav() above, not the audio ISR.
bool SaveGranularPreset(int                                       slot,
                        const GranularEngine::GranularPresetData& preset,
                        const float*                              audio_l,
                        const float*                              audio_r,
                        size_t                                    audio_len,
                        ProgressFn                                on_progress = nullptr);
// out_audio_l/r must have room for at least audio_capacity samples each
// -- the file's own audio_len is clamped to that before reading, same
// "never write past what the caller actually owns" reasoning as every
// other buffer-filling call in this project. *out_audio_len is set to
// however many samples were actually read.
bool LoadGranularPreset(int                                  slot,
                        GranularEngine::GranularPresetData* out_preset,
                        float*                               out_audio_l,
                        float*                               out_audio_r,
                        size_t                                audio_capacity,
                        size_t*                               out_audio_len,
                        ProgressFn                            on_progress = nullptr);
int  ListGranularPresets(int* out_numbers, int max_out);
int  NextFreeGranularPresetSlot();
// No factory range to guard against here (see kMaxGranularPresets's own
// comment) -- see DeleteSlot()/DuplicateSlot() above for the general
// shape, including the same gap-closing renumbering and
// loaded_slot_inout follow-along. Duplicating copies the whole file
// (params + captured audio) in one chunked byte-for-byte pass, not
// through GranularEngine at all.
bool DeleteGranularPreset(int slot, int* loaded_slot_inout = nullptr);
bool DuplicateGranularPreset(int slot, int* out_new_slot, ProgressFn on_progress = nullptr);

// Grains Preset-page library: folders under GRAINS/ holding the same
// audio-inclusive preset files as the numbered user saves above, but
// listed by name. GRAINS/PRESETS/ holds the starter presets (any *.DAT
// dropped in there shows up); saved captures fill GRAINS/CAPTURED1,
// CAPTURED2, ... (CAPnnn.DAT, kSampleBankCapacity-style 32 per folder,
// the next folder created automatically once every existing one is full).
constexpr const char* kGranularStarterFolder  = "PRESETS";
constexpr int         kGranularCapturedPerBank = 32;
constexpr int         kMaxGranularCapturedBanks = 99;
constexpr int         kMaxGranularLibNameLen   = 60; // same as kMaxImportWavNameLen
// Existing CAPTUREDn numbers, ascending.
int  ListGranularCapturedBanks(uint8_t* out_numbers, int max_out);
// *.DAT files in GRAINS/<folder>, sorted by name. Names longer than
// kMaxGranularLibNameLen are skipped, not truncated.
int  ListGranularLibraryItems(const char* folder, char out_names[][kMaxGranularLibNameLen + 1],
                              int max_out);
bool LoadGranularLibraryPreset(const char*                         folder,
                               const char*                         name,
                               GranularEngine::GranularPresetData* out_preset,
                               float*                              out_audio_l,
                               float*                              out_audio_r,
                               size_t                              audio_capacity,
                               size_t*                             out_audio_len,
                               ProgressFn                          on_progress = nullptr);
// Writes into the lowest CAPTUREDn with a free CAPnnn.DAT, creating the
// next folder when all are full. out_folder/out_name receive where it
// landed (e.g. "CAPTURED1", "CAP004.DAT").
bool SaveGranularCapture(const GranularEngine::GranularPresetData& preset,
                         const float*                              audio_l,
                         const float*                              audio_r,
                         size_t                                    audio_len,
                         char*                                     out_folder,
                         size_t                                    out_folder_size,
                         char*                                     out_name,
                         size_t                                    out_name_size,
                         ProgressFn                                on_progress = nullptr);
// Global:SdMgmt's view into Grains captures -- CAPnnn.DAT (n = 1..32) in
// GRAINS/CAPTURED<bank>, same delete-then-close-the-gap and duplicate-
// into-the-same-folder shape as DeleteSampleSynthCapture()/
// DuplicateSampleSynthCapture().
bool DeleteGranularCapture(int bank, int n);
bool DuplicateGranularCapture(int bank, int n, int* out_new_n, ProgressFn on_progress = nullptr);

// Dexed presets: parameters only, no audio -- same shape as the
// removed FmSynth/PadSynth presets, including the same factory range:
// 1..DexedSynth::GetNumFactoryPresets() are the permanently read-only,
// firmware-embedded factory presets (real DX7 patches, see
// dexed_factory_data.h/.cpp) -- LoadDexedPreset() serves those directly
// without touching the card, and SaveDexedPreset()/
// NextFreeDexedPresetSlot() never target them. User saves start right
// after that range. Unlike the removed Fm/Pad presets' fixed factory
// count, this range is sized from the real embedded data (704 patches
// as of writing) rather than a compile-time constant.
// User range above the 3129 factory presets: 2371 slots -- room for all
// kMaxDexedImportFolders (64) imports of 32 voices plus a few hundred of
// your own saves. (Was 3500, which 8 imported banks already 2/3 filled.)
constexpr int kMaxDexedPresets = 5500;

bool SaveDexedPreset(int slot, const DexedSynth::DexedPresetData& preset);
bool LoadDexedPreset(int slot, DexedSynth::DexedPresetData* out_preset);
// Lists existing USER slots only (ascending) -- factory presets are
// always available and aren't part of this scan. Same shape as
// ListSlots().
int  ListDexedPresets(int* out_numbers, int max_out);
// Lowest free USER slot, or -1 if the whole range is full/no card.
// search_from (optional): resume scanning from this slot instead of
// the very start of the USER range -- lets a caller doing several
// consecutive allocations (see Ui::TriggerDexedSyxImport()'s own
// SysEx-bank-import loop) advance a single forward sweep across the
// whole batch (each call starting where the last one left off, i.e.
// search_from = previous result + 1) instead of re-scanning from
// scratch every time, which turns an O(n) find into an O(n^2) one as
// the USER range grows. Values at or below the factory range are
// clamped up to its own real start, same as omitting this entirely.
int  NextFreeDexedPresetSlot(int search_from = -1);
// Refuses factory slots (1..DexedSynth::GetNumFactoryPresets() aren't
// files at all) same as SaveDexedPreset() -- see DeleteSlot()/
// DuplicateSlot() above for the general shape, including the same
// gap-closing renumbering and loaded_slot_inout follow-along. The
// factory range itself is never deleted/compacted through this path --
// see CompactSlotsAfterDelete()'s own doc comment in the .cpp for why
// that renumbering scheme is fundamentally incompatible with a fixed,
// ordered factory bank.
bool DeleteDexedPreset(int slot, int* loaded_slot_inout = nullptr);
bool DuplicateDexedPreset(int slot, int* out_new_slot, ProgressFn on_progress = nullptr);

// Ensemble presets: parameters only, no audio -- same flat shape as
// Dexed presets above, just with a much smaller factory range (8 hand-
// authored patches, see ensemble_factory_data.h/.cpp, vs. Dexed's
// thousands): slots 1..EnsembleSynth::GetNumFactoryPresets() are the
// permanently read-only, firmware-embedded factory bank --
// LoadEnsemblePreset() serves those directly without touching the card,
// and SaveEnsemblePreset()/NextFreeEnsemblePresetSlot() never target
// them. User saves start right after that range.
constexpr int kMaxEnsemblePresets = 107; // user saves ENSM9..ENSM107
// User saves are numbered from 9 for good -- whatever the size of the
// factory bank. (They used to start right after the factory presets,
// which happened to be 8; adding factory presets would have hidden
// ENSM9 onwards.) Factory presets aren't files and are loaded straight
// from EnsembleSynth::GetFactoryPreset().
constexpr int kEnsembleFirstUserSlot = 9;

bool SaveEnsemblePreset(int slot, const EnsembleSynth::EnsemblePresetData& preset);
bool LoadEnsemblePreset(int slot, EnsembleSynth::EnsemblePresetData* out_preset);
// Lists existing USER slots only (ascending) -- same shape as ListDexedPresets().
int  ListEnsemblePresets(int* out_numbers, int max_out);
// Lowest free USER slot, or -1 if the whole range is full/no card.
int  NextFreeEnsemblePresetSlot();
// Same shape as DeleteDexedPreset()/DuplicateDexedPreset() -- refuses
// factory slots, same gap-closing renumbering.
bool DeleteEnsemblePreset(int slot, int* loaded_slot_inout = nullptr);
bool DuplicateEnsemblePreset(int slot, int* out_new_slot, ProgressFn on_progress = nullptr);

// Importing a user-supplied DX7 SysEx file (.syx, from a computer,
// dropped into DEXED IMPORT/ on the SD card) as new Dexed USER presets
// -- a separate folder from Grains' own GRAINS IMPORT/ so the two file
// types never collide in one listing. Two real-world SysEx shapes are
// recognized, both fully validated (Yamaha ID byte, message length,
// 7-bit checksum) before anything is trusted:
//   Single Voice Dump (format 0): exactly one patch, 163 bytes on the
//     wire (F0 43 0S 00 01 1B <155 bytes, already in this project's own
//     unpacked layout -- see dexed_sysex.h's own doc comment for why
//     the single-voice wire format and DexedSynth's internal 156-byte
//     patch_[] layout are byte-for-byte the same thing> <checksum> F7).
//   32-Voice Bulk Dump (format 9): a full bank, 4104 bytes on the wire
//     (F0 43 0S 09 20 00 <32*128 packed-format bytes, the same layout
//     DexedSysex::UnpackVoice() already expands for the embedded
//     factory banks> <checksum> F7).
// Anything else (wrong ID byte, wrong length, bad checksum, an
// unrecognized format number) is cleanly refused (GetLastError()
// reports why), never guessed at.
constexpr int kMaxImportSyxNameLen = 60;
// Same shape/rules as ListImportWavFiles() below (ascending directory
// order, skips macOS AppleDouble sidecars, skips names longer than
// kMaxImportSyxNameLen rather than truncating), scoped to DEXED IMPORT/
// and .syx/.SYX instead of GRAINS IMPORT/ and .wav/.WAV.
int  ListImportSyxFiles(char out_names[][kMaxImportSyxNameLen + 1], int max_out);
constexpr int kMaxSyxBulkVoices = 32;
// Reads and validates filename (inside DEXED IMPORT/), decodes every voice
// it contains into out_presets[0..*out_count-1] (patch bytes only --
// reverb_send01/output_level01/filter/pan all take DexedPresetData's
// own defaults, same as GetFactoryPreset()) and sets *out_count to 1
// (single dump) or 32 (bulk dump). Caller is responsible for actually
// writing each one to a real user slot (see
// Ui::TriggerDexedSyxImport()) -- this function only reads and decodes
// the file, it never touches the DEXED/ preset store itself.
bool ImportDexedSyx(const char*                  filename,
                    DexedSynth::DexedPresetData out_presets[kMaxSyxBulkVoices],
                    int*                         out_count);

// One folder created per imported SysEx bank (see ImportDexedSyx()
// above) -- its own name (derived from the imported file's own name,
// since the SysEx data itself carries no overall "bank name" field,
// only each voice's own 10-character name) plus the contiguous range
// of USER slots that import filled. A flat, fixed-size-record file
// (DEXED/IMPORTS.DAT, no header -- record count is just file size /
// sizeof(DexedImportFolder)) rather than a versioned/magic'd format
// like every other *PresetData store here, since there's nothing about
// this shape that could ever need to change compatibly -- if it ever
// does, this whole file can simply be deleted and rebuilt by
// re-importing (the underlying preset slots themselves are untouched).
struct DexedImportFolder
{
    char    name[24];
    int32_t first_slot;
    int32_t last_slot;
};
constexpr int kMaxDexedImportFolders = 64;
// Appends one new import-folder record -- called once per completed
// ImportDexedSyx()+save batch (see Ui::TriggerDexedSyxImport()), never
// per-voice. Returns false (GetLastError() explains) if the manifest
// couldn't be written -- the underlying preset slots are already saved
// either way at that point, so a false here means "the folder grouping
// didn't record," not "the import itself failed."
bool SaveDexedImportFolder(const char* name, int first_slot, int last_slot);
// Reads every recorded import folder, ascending (oldest import first,
// so folder order in the browser matches import order). 0 (not an
// error) if no imports have ever been recorded, i.e. the file doesn't
// exist yet.
int ListDexedImportFolders(DexedImportFolder* out, int max_out);

// Importing a user-supplied WAV file (from a computer, dropped into
// GRAINS IMPORT/ on the SD card) as Grains capture audio -- a third capture
// source alongside Direct Record and From Layer. Accepts 16/24/32-bit
// PCM or 32-bit IEEE float, mono or stereo, and any of the common real-
// world WAV sample rates (8000/11025/16000/22050/24000/32000/44100/
// 48000/88200/96000/176400/192000 Hz) -- anything other than 48000 Hz is
// resampled to the native 48000 Hz engine rate with an exact-ratio
// Catmull-Rom resampler (upsampling for rates below 48kHz, the same
// technique ExportWav() already uses in the other direction; a genuine
// downsample for rates above 48kHz gets a real anti-aliasing prefilter
// first). Anything else (an unlisted sample rate, a non-PCM/float
// format) is cleanly refused (GetLastError() reports why) rather than
// misread.
constexpr int kMaxImportWavNameLen = 60;
// Fills out_names ascending (directory order) with .wav/.WAV filenames
// found in GRAINS IMPORT/ on the SD root, returns how many were found (up to
// max_out). A name longer than kMaxImportWavNameLen is SKIPPED entirely
// rather than truncated -- an earlier version truncated it instead, which
// silently produced a listed name that didn't match any real file on
// disk (ImportWav() would then fail to open it, FR_NO_FILE) since
// truncating can chop off the ".wav" extension or land mid-name.
// Realistic sample filenames are well under this length in practice.
// Also skips macOS "._name.wav" AppleDouble sidecar files -- these get
// silently created alongside every real file when Finder (or many other
// macOS copy tools) writes to a non-HFS+ volume like a FAT32 SD card;
// they end in .wav and so pass the extension filter, but their content
// isn't a real WAV file (fails ImportWav()'s own RIFF magic check).
int  ListImportWavFiles(char out_names[][kMaxImportWavNameLen + 1], int max_out);
// out_l/out_r must have room for at least audio_capacity samples each --
// the file's own length (after any resampling) is truncated to that,
// same "never write past what the caller owns" rule as
// LoadGranularPreset(). *out_len is set to however many samples were
// actually produced. folder defaults to "GRAINS IMPORT" (Grains' own,
// unchanged for every existing call site) -- SampleSynth's own loader
// passes kSampleSynthFolder instead, same RIFF-decode/resample logic
// either way, just a different SD folder (see kSampleSynthFolder's own
// comment for why this is a separate folder rather than sharing it).
bool ImportWav(const char* filename,
               float*      out_l,
               float*      out_r,
               size_t      audio_capacity,
               size_t*     out_len,
               ProgressFn  on_progress = nullptr,
               const char* folder      = "GRAINS IMPORT");

// SampleSynth's own SD folder for user-supplied WAV files -- deliberately
// separate from Grains' GRAINS IMPORT/ and Dexed's DEXED IMPORT/, same
// "never mix different engines' own file listings into one folder"
// reasoning ImportSyxFilename()'s own comment already documents.
constexpr const char* kSampleSynthFolder        = "SAMPLES";
constexpr int         kMaxSampleSynthWavNameLen = kMaxImportWavNameLen;
// --- Numbered "bank" folders under kSampleSynthFolder -------------------
// Every kind of SampleSynth content lives in numbered folders of up to
// kSampleBankCapacity items each -- SOUNDS1, SOUNDS2, ..., CAPTURED1, ...,
// KITS1, ..., BREAKS1, ... -- so a card can hold far more than one
// folder's worth (the browser and SdMgmt only ever list one bank at a
// time). Imports and saves fill the lowest-numbered bank with space and
// create the next one automatically once every existing bank is full.
// Sounds/Captured items are files (.wav, CAPnnn.wav); Kits/Breaks items
// are folders, one per kit, holding its pad WAVs plus (usually) KIT.DAT
// (see SaveSampleSynthKit()); Multi items are folders of note-named WAVs
// (see BuildMultiSampleZones()).
enum class SampleBank : uint8_t
{
    Sounds,
    Captured,
    Kits,
    Breaks,
    // Multi-samples: one folder per instrument, holding a WAV per sampled
    // note, the note in each name -- see BuildMultiSampleZones().
    Multi,
};
// Banks whose items are folders (Kits, Breaks, Multi), not files.
inline bool IsFolderSampleBank(SampleBank b)
{
    return b == SampleBank::Kits || b == SampleBank::Breaks || b == SampleBank::Multi;
}
constexpr int kSampleBankCapacity = 32; // items a save/import fills a bank to
constexpr int kMaxSampleBanks     = 99; // per SampleBank type
// "SOUNDS", "CAPTURED", "KITS", "BREAKS", "MULTISMP".
const char* SampleBankPrefix(SampleBank b);
// Folder name relative to kSampleSynthFolder, e.g. "KITS3".
void SampleBankFolder(SampleBank b, int n, char* out, size_t out_size);
// Existing bank numbers of one type, ascending -- one directory scan of
// kSampleSynthFolder, not a per-number f_stat() loop.
int ListSampleBanks(SampleBank b, uint8_t* out_numbers, int max_out);
// Items in one bank, sorted by name (see this section's own comment for
// what counts as an item per type). Names longer than
// kMaxSampleSynthWavNameLen are skipped, not truncated, same as
// ListImportWavFiles().
int ListSampleBankItems(SampleBank b, int n, char out_names[][kMaxSampleSynthWavNameLen + 1],
                        int max_out);
// Where the next new item of this type goes (creating the bank folder if
// needed): `prefer` if it has room (Duplicate keeps a copy in its own
// bank); else the Import page's chosen destination (SetImportDestination())
// if it has room; else the HIGHEST-numbered bank if it has room; else a new
// bank numbered one above it. Older banks -- including ones a user made,
// or ones with gaps left by deletions -- are never filled automatically.
// Returns -1 (error set) if bank kMaxSampleBanks is reached or mkdir fails.
int FindSampleBankWithSpace(SampleBank b, int prefer = 0);
// The Import page's destination for each type, 0 = none (the rule above).
void SetImportDestination(SampleBank b, int bank_number);
void ClearImportDestinations();
// The number a "New bank" gets: one above the highest existing (1 if
// none), or -1 past kMaxSampleBanks.
int NextNewSampleBank(SampleBank b);
// Items currently in one bank (free = kSampleBankCapacity - this).
int SampleBankItemCount(SampleBank b, int n);

// Several sample-pack files state their own recorded pitch right in the
// filename (e.g. "SKOGSFIOL C4.wav", "DRAGSPEL A2.wav") -- scans for a
// trailing "<NoteLetter>[#/b]<octave>" token (the last space-or-dot-
// delimited word before the extension) and converts it to a MIDI note
// number using the same C4==60 (middle C) convention the pack's own
// naming already implies. Returns true and writes *out_note on a match;
// returns false (leaving *out_note untouched) for filenames with no
// such token, so the caller can fall back to a plain default instead of
// silently mis-pitching a sample that never claimed a note in the first
// place. Shared by Ui::LoadSampleSynthFileIntoEngine() (reading a
// filename at load time) and ImportAifSamplerPack() (reading the
// *source* filename when a .aif carries no op-1 chunk of its own).
bool ParseNoteNameFromFilename(const char* filename, uint8_t* out_note);

// SampleSynth's own "kit" -- a small manifest (which WAV file maps to
// which slot, chromatic/fixed-pitch, note range, volume) plus the
// engine's own knob settings. Mirrors Dexed/Grains' own "preset !=
// captured audio" split, but unlike Grains' own preset (which embeds
// its single captured buffer directly in the preset file), a kit only
// ever stores filenames -- the referenced WAVs stay as plain files in
// SAMPLES/, since one kit can reference up to SampleSynth::kMaxVoices
// different files and embedding would mean duplicating audio that's
// often shared across several kits.
struct SampleSynthKitSlot
{
    char    filename[kMaxSampleSynthWavNameLen + 1] = {}; // empty = unused slot
    bool    chromatic  = false;
    uint8_t root_note  = 60;
    uint8_t low_note   = 0;
    uint8_t high_note  = 127;
    float   volume01   = 0.8f;
};
constexpr int kMaxSampleSynthSlots      = 24; // matches SampleSynth::kMaxSampleSlots
constexpr int kMaxSampleSynthKitNameLen = 23; // a short display label, not a filename
// A kit is identified by its folder relative to kSampleSynthFolder --
// "<bank folder>/<kit folder>", e.g. "KITS1/lofi2-4071" -- never by a
// number, so deleting one kit can't renumber (and silently retarget)
// every Drum User preset pointing at the kits after it.
constexpr int kMaxSampleSynthKitRefLen = 40;

// Writes SAMPLES/<kit_ref>/KIT.DAT (the folder must already exist).
// name is a short display label, NOT a filename. Each slot's filename is
// relative to the kit's own folder (e.g. "01.wav").
bool SaveSampleSynthKit(const char* kit_ref, const char* name,
                         const SampleSynth::SampleSynthPresetData& params,
                         const SampleSynthKitSlot slots[kMaxSampleSynthSlots]);
bool LoadSampleSynthKit(const char* kit_ref, char* out_name, size_t out_name_size,
                         SampleSynth::SampleSynthPresetData* out_params,
                         SampleSynthKitSlot out_slots[kMaxSampleSynthSlots]);
// A Multi bank item, ref "MULTISMPn/<folder>": one zone per WAV whose
// name states a note (see ParseNoteNameFromFilename()), sorted by note,
// each reaching halfway to its neighbours (the lowest and highest to the
// keyboard's ends). Filenames are relative to the folder. Up to
// kMaxSampleSynthSlots zones; WAVs with no note, a repeated note, or past
// the limit are counted in *out_skipped. false if no WAV states a note.
bool BuildMultiSampleZones(const char* ref, SampleSynthKitSlot out_slots[kMaxSampleSynthSlots],
                           int* out_skipped);
// "SAMPLES/<kit_ref>" -- the folder a kit's own slot filenames are
// relative to (what Ui passes to ImportWav() as its folder).
void SampleSynthKitDir(const char* kit_ref, char* out, size_t out_size);
// Which bank type a kit_ref lives in (Kits or Breaks), from its prefix.
SampleBank SampleSynthKitBank(const char* kit_ref);

// The old multi-sample format: a ".SND" manifest in a SOUNDSn bank listing
// zones (SampleSynthKitSlot each, filenames relative to SAMPLES/, usually
// ZONES/<name>/). Only read now to convert one into a MULTISMPn folder --
// see MigrateSndSoundsToMulti(). subfolder is the bank folder.
bool LoadSampleSynthSound(const char* filename, SampleSynthKitSlot out_slots[kMaxSampleSynthSlots],
                           const char* subfolder = nullptr);

// SMPVO's own real User preset -- a reference (which category + which
// filename), never a copy of the audio itself, plus a full settings
// snapshot (Filter/Env/Mix/FX, and the Imp-bank's own Tune/Mono-Poly --
// see SampleSynthParamPage::Tune's own K2/B2). Replaces the old
// SampleSynthFavorite bookmark (reference only, no settings -- "load a
// Sound and get none of your settings back with it," a real reported
// gap) now that this covers the same job plus the params. Lives on
// SampleSynthParamPage::User (Voice-only, see Ui::IsSampleSynthPageValidForMode()).
struct SampleSynthVoiceUserPreset
{
    char    category[17] = {}; // a SOUNDSn/CAPTUREDn bank folder, e.g. "SOUNDS2"
    char    filename[kMaxSampleSynthWavNameLen + 1] = {};
    SampleSynth::SampleSynthPresetData params; // mono_choke unused here -- SMPVO uses chromatic Mono/Poly instead
    int8_t  tune_semis = 0;
    bool    mono       = false;
};
constexpr int kMaxSampleSynthVoiceUserPresets = 99; // same user headroom as Granular/Ensemble presets

bool SaveSampleSynthVoiceUserPreset(int slot, const SampleSynthVoiceUserPreset& preset);
bool LoadSampleSynthVoiceUserPreset(int slot, SampleSynthVoiceUserPreset* out_preset);
// Lists existing slots only (ascending) -- same shape as ListEnsemblePresets().
int  ListSampleSynthVoiceUserPresets(int* out_numbers, int max_out);
// Lowest free slot, or -1 if the whole range is full/no card.
int  NextFreeSampleSynthVoiceUserPresetSlot();
// No factory tier -- every slot is a user save. Global:SdMgmt's own
// Delete/Duplicate on the SMPVO User box.
bool DeleteSampleSynthVoiceUserPreset(int slot, int* loaded_slot_inout = nullptr);
bool DuplicateSampleSynthVoiceUserPreset(int slot, int* out_new_slot,
                                           ProgressFn on_progress = nullptr);

// SMPDR's own counterpart -- references a real Kit by folder (see
// kMaxSampleSynthKitRefLen, never copied or re-sliced) plus a full settings snapshot
// including Setup's own Break Divide/Tune (break_divide_count/
// break_tune_semis are otherwise pure live/session state, never saved
// with the Kit itself -- see that field's own comment in sample_synth.h's
// history). Lives on SampleSynthParamPage::User (Drum-only).
struct SampleSynthDrumUserPreset
{
    char    kit_ref[kMaxSampleSynthKitRefLen] = {}; // empty = none
    SampleSynth::SampleSynthPresetData params; // Filter/Env/Mix/FX/Mono-Choke
    uint8_t break_divide_count = 0;
    int8_t  break_tune_semis   = 0;
};
constexpr int kMaxSampleSynthDrumUserPresets = 99;

bool SaveSampleSynthDrumUserPreset(int slot, const SampleSynthDrumUserPreset& preset);
bool LoadSampleSynthDrumUserPreset(int slot, SampleSynthDrumUserPreset* out_preset);
int  ListSampleSynthDrumUserPresets(int* out_numbers, int max_out);
int  NextFreeSampleSynthDrumUserPresetSlot();
// No factory tier -- every slot is a user save. Global:SdMgmt's own
// Delete/Duplicate on the SMPDR User box.
bool DeleteSampleSynthDrumUserPreset(int slot, int* loaded_slot_inout = nullptr);
bool DuplicateSampleSynthDrumUserPreset(int slot, int* out_new_slot,
                                          ProgressFn on_progress = nullptr);

// Writes a trimmed Capture/Trim selection out as a real, plain 16-bit
// PCM stereo WAV under SAMPLES/<category_folder>/ (e.g. "CAPTURED1"),
// auto-numbered CAPnnn.wav (001-032, same "scan then pick the first
// free one" idiom as NextFreeGranularPresetSlot(), just over filenames
// instead of a fixed numbered-file scheme) -- this is the whole reason
// a captured Sound becomes an ordinary file afterward (browsable,
// favoritable, usable in a Kit) instead of a second, Grains-style
// preset format that embeds audio directly. Always 48kHz (every
// capture source -- Direct Record, From Layer, Import -- already
// produces 48kHz, ImportWav() resamples on the way in), stereo.
// Returns false (category full, no free CAPnnn slot, or a real SD
// error) without creating anything -- the caller tries the next
// category, or reports "Captured full" once both are exhausted. On
// success, out_filename receives the chosen name (e.g. "CAP003.wav").
bool SaveSampleSynthCapture(const char* category_folder, const float* buf_l, const float* buf_r,
                             size_t len, char* out_filename, size_t out_filename_size);

// Global:SdMgmt's own view into Captures -- deletes/duplicates real
// CAPnnn.wav files (001-032) within ONE category_folder (a CAPTUREDn
// bank), same shape as DeleteSlot()/DuplicateSlot() elsewhere in this
// file. Listing goes through ListSampleBankItems().
bool DeleteSampleSynthCapture(const char* category_folder, int n);
bool DuplicateSampleSynthCapture(const char* category_folder, int n, int* out_new_n,
                                  ProgressFn on_progress = nullptr);

// --- OP-1 .aif pack import ---------------------------------------------
// Staging folders for raw downloaded OP-1 packs (op1fun or the OP-1
// pack builder) -- SD root, separate from kSampleSynthFolder itself,
// user-managed drop-off points. Capped at kMaxSampleSynthImportPackFiles
// (32) like every other real-folder listing in this project.
constexpr const char* kSampleDrumImportFolder   = "DRUM PACK IMPORT";
// Drum packs are sorted by the folder the user drops them in -- nothing
// is classified automatically (no slice-length rule can tell a finely
// chopped break from a kit). Each has its own Done/.
constexpr const char* kSampleKitImportFolder    = "DRUM PACK IMPORT/KITS";
constexpr const char* kSampleBreakImportFolder  = "DRUM PACK IMPORT/BREAKS";
// Where drum packs went before that split: loose in DRUM PACK IMPORT/,
// and the separate BREAK PACK IMPORT/. Never imported from -- the
// Import page only counts them so it can say to move them.
constexpr const char* kSampleLegacyBreakImportFolder = "BREAK PACK IMPORT";
constexpr const char* kSampleSoundImportFolder  = "SOUND PACK IMPORT";
// Per listing pass, not per Import press -- Ui re-lists and keeps going
// until nothing unprocessed is left (see Ui::TriggerSampleSynthPackImport()).
constexpr int         kMaxSampleSynthImportPackFiles = 32;
// Makes DRUM PACK IMPORT/, its KITS/ and BREAKS/, and their Done/ folders
// (harmless if present).
void EnsureDrumImportFolders();

// Lists .aif files directly under `folder` (one of the two staging
// folders above, SD root -- NOT under kSampleSynthFolder), up to
// kMaxSampleSynthImportPackFiles. Same skip rules (AppleDouble
// sidecars, name-too-long) as ListImportWavFiles()'s own. skip_first
// passes over that many matching files first -- Import uses it to step
// past files that failed earlier in the same press (they stay in place
// for the next press; directory order keeps them ahead of the rest).
int ListAifFiles(const char* folder, char out_names[][kMaxSampleSynthWavNameLen + 1],
                  int max_out, int skip_first = 0);

// Decodes one AIFF file (FORM/COMM/SSND, 16-bit PCM, big-endian, mono
// or stereo) -- same contract as ImportWav() (resamples through the
// same Catmull-Rom path when the real rate isn't 48000), just for the
// different container OP-1 exports use.
bool ImportAif(const char* filename, float* out_l, float* out_r, size_t audio_capacity,
                size_t* out_len, const char* folder);

// Global:SdMgmt's own Delete/Duplicate on the Kits box. Delete removes
// the kit's whole folder (KIT.DAT and every pad WAV). Duplicate copies
// it into the lowest bank of the same type (Kits/Breaks) with space, as
// "<name> 2" (or the next free number), and returns the new kit_ref.
bool DeleteSampleSynthKit(const char* kit_ref);
// Global:SdMgmt's SMPIMP -> Voice view: one imported Sound (.wav) in a
// SOUNDSn bank. Duplicate copies into the same bank as "<name> 2",
// "<name> 3", ... (A Multi bank's folders use the Kit functions above.)
bool DeleteSampleSound(const char* bank_folder, const char* name);
bool DuplicateSampleSound(const char* bank_folder, const char* name, char* out_name,
                          size_t out_name_size, ProgressFn on_progress = nullptr);
bool DuplicateSampleSynthKit(const char* kit_ref, char* out_new_ref, size_t out_new_ref_size,
                              ProgressFn on_progress = nullptr);

// Decodes filename (expected inside `folder`, one of the staging
// folders above) as an OP-1 "op-1" APPL-chunk drum patch: slices its
// one audio stream at the real splice points (see this file's own
// commit history for the full op-1 JSON schema write-up) -- or, with no
// op-1 drum data at all (a plain recording, or a sampler patch), takes
// the whole file as one slice. Files the result as a Break if is_break,
// otherwise as a Kit: a new folder
// named after the pack in the lowest bank of that type with space,
// holding each real slice as 01.wav, 02.wav, ... plus KIT.DAT. scratch_l/r (and their
// shared scratch_capacity) are the caller's own large decode buffer --
// the whole pack's own audio stream is decoded into them once via
// ImportAif() before being sliced, same "reuse the existing SDRAM
// arena, don't allocate a second one" convention
// Ui::TriggerLoadSampleSynthKit() already follows for the exact same
// arena (the caller passes sample_synth_arena_l_/r_ straight through,
// safe since no Kit can be loading mid-import). Returns false
// (out_status explains why: wrong type, corrupt file, Kit range full)
// without writing anything partial.
// progress_cb (optional) is called after each of the up to
// kMaxSampleSynthSlots pad slots is processed (real slice written or
// padding skipped) -- the up-to-24 separate SD writes this does are
// the real cost of importing one drum pack, so this is what makes the
// Import page's own progress bar actually move smoothly within a
// single file instead of sitting still for however long that file
// takes, then jumping straight to the next file's own share.
bool ImportAifDrumPack(const char* filename, const char* folder, bool is_break,
                        float* scratch_l, float* scratch_r, size_t scratch_capacity,
                        char* out_status, size_t out_status_size,
                        ProgressFn progress_cb = nullptr);

// Decodes filename as an OP-1 "sampler" patch -- or, if it carries no
// op-1 chunk at all, a plain recording -- into one new Sound: a plain
// .wav written into the lowest SOUNDSn bank with space, named
// with its real root note baked in (same convention
// ParseNoteNameFromFilename() already reads) so it loads through the
// exact same plain-.wav path every other Sound already does -- no new
// runtime loading code needed. Root note comes from the patch's own
// base_freq when present, otherwise from a note name already in the
// source filename, otherwise defaults to C4 (60). scratch_l/r are the
// same shared decode buffer ImportAifDrumPack() uses.
bool ImportAifSamplerPack(const char* filename, const char* folder, float* scratch_l,
                           float* scratch_r, size_t scratch_capacity, char* out_status,
                           size_t out_status_size);

// Moves a successfully-imported source file into <folder>/Done/ so a
// later Import never reprocesses it -- the move itself is the only
// "already imported" state kept, no separate tracking file.
bool MoveToImportDoneFolder(const char* folder, const char* filename);

// Renders the current in-memory performance (one full shared loop length,
// every layer's real filter/character-effect/reverb chain applied,
// same as live playback) to a new stereo 16-bit PCM WAV file. Two
// independent output modes, selected by for_microdexed:
//   false: full-quality native 48000 Hz, under "EXPORTS/" -- general
//     purpose, kept out of the SD root so it never shows up as a load
//     target in ListSlots().
//   true: same performance/DSP chain (the chain itself always runs at
//     native 48kHz -- see ExportWav()'s sample_rate local), but the
//     final output is resampled to 44100 Hz (Resampler48to44_1, in the
//     .cpp) and written under "custom/" -- that folder name is
//     required, not cosmetic: MicroDexed Touch scans its SD card for a
//     folder literally named "custom" and expects 44100 Hz PCM.
// Each mode has its own independent EXPnnn numbering sequence.
// Master volume and the metronome click are deliberately NOT included
// (see performance_store.cpp); the master filter and the shared reverb/
// delay buses (reverb_size01/delay_time01/delay_feedback01 -- see
// Ui::GetReverbSize01()/GetDelayTime01()/GetDelayFeedback01()) all are.
// project_speed
// is the live vari-speed multiplier (see Ui::GetProjectSpeed()) -- unlike
// master volume, this one IS captured into the export as-is, on purpose:
// vari-speed is something a user dials in deliberately as part of a
// performance (e.g. the classic "record fast, play back normal" tape
// trick), not an incidental monitor-level knob position, so baking in
// whatever it's currently set to when Export is pressed is the wanted
// behavior, not a leak of live-only state. Blocking, like Save()/Load()
// -- call from main()'s loop, not the audio callback, and with
// g_audio_suspended held true for the whole call (see Ui::TriggerExport())
// since this drives extra LooperLayer::Process() calls from outside the
// real ISR. Refuses to run while any layer is Recording or ArmedCountIn,
// or if nothing has been recorded yet.
bool ExportWav(TempoClock&  tempo,
               LooperLayer* layers,
               int          num_layers,
               FilterMode   master_filter_mode,
               float        master_filter_cutoff01,
               float        master_filter_res01,
               float        reverb_size01,
               float        reverb_predelay01,
               float        reverb_prefilter01,
               float        reverb_input_diffusion1_01,
               float        reverb_input_diffusion2_01,
               float        reverb_decay_diffusion01,
               float        reverb_damping01,
               const SharedDelay::Settings& delay,
               float        delay_to_reverb01,
               bool         for_microdexed,
               float        project_speed,
               ProgressFn   on_progress = nullptr);

// User-settable startup defaults -- every *global* setting, deliberately
// no per-layer ones (see Global:Tempo's Button2 hold). Stored as a
// single small "PREFS.DAT" in the SD root, reusing the same on-disk
// header shape Save()/Load() already use for these exact fields (just
// without any layers/audio following it) rather than a second format.
//
// LoadPrefs() returns false (every output left untouched) if the card
// is missing, no PREFS.DAT exists yet, or it doesn't look like a prefs
// file (bad magic/version, e.g. left over from an older firmware) --
// none of those are errors worth surfacing; the caller's own hardcoded
// defaults should just stand.
bool SavePrefs(TempoClock&  tempo,
               float        master_volume01,
               bool         bypass,
               FilterMode   master_filter_mode,
               float        master_filter_cutoff01,
               float        master_filter_res01,
               float        reverb_size01,
               float        bypass_reverb_send01,
               float        reverb_predelay01,
               float        reverb_prefilter01,
               float        reverb_input_diffusion1_01,
               float        reverb_input_diffusion2_01,
               float        reverb_decay_diffusion01,
               float        reverb_damping01,
               int          dexed_midi_channel,
               int          granular_midi_channel,
               int          ensemble_midi_channel,
               int          sample_synth_midi_channel,
               const DelayState& delay);

// Same v11..v16 backward compatibility as Load() -- see its own comment.
bool LoadPrefs(float*      out_bpm,
               int*        out_bars,
               float*      out_master_volume01,
               bool*       out_bypass,
               FilterMode* out_master_filter_mode,
               float*      out_master_filter_cutoff01,
               float*      out_master_filter_res01,
               float*      out_reverb_size01,
               float*      out_bypass_reverb_send01,
               bool*       out_metronome_enabled,
               float*      out_metronome_vol01,
               float*      out_reverb_predelay01,
               float*      out_reverb_prefilter01,
               float*      out_reverb_input_diffusion1_01,
               float*      out_reverb_input_diffusion2_01,
               float*      out_reverb_decay_diffusion01,
               float*      out_reverb_damping01,
               int*        out_dexed_midi_channel,
               int*        out_granular_midi_channel,
               int*        out_ensemble_midi_channel,
               int*        out_sample_synth_midi_channel,
               DelayState* inout_delay);

} // namespace PerformanceStore
