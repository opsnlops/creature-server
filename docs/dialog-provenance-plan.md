# Dialog sound provenance & subdir access — plan

Covers **creature-server #46** (dialog-subdir sounds are invisible to the sound
API) and **#47** (dialog WAVs are anonymous UUID files with no link back to their
script). Done together because both are about the `dialog/` subdir being a
second-class citizen: #46 makes those files *discoverable*, #47 makes them
*self-describing*.

## Background

Permanent dialog renders write `<jobId>.wav` into a `dialog/` subdirectory of the
permanent sound root (`kPermanentDialogSubdir` in `JobWorker.cpp`). The animation's
`metadata.sound_file` carries the **relative** path `dialog/<jobId>.wav`, which the
storage facade (`resolveSoundPath`) resolves back to an absolute path. This is the
storage layer's native model — `Persistence::Permanent` is documented to store
relative paths and `allocateSoundPath` takes a `subdir`.

Two things never got updated to match:

1. `SoundService::getAllSounds` walks the root with a **non-recursive**
   `fs::directory_iterator` → dialog files never appear in `GET /api/v1/sound`.
2. `SoundController::getSound` / `getShareableSound` resolve a **flat**
   `root + "/" + filename` path, and `sanitizeFilename` rejects any `/` → a dialog
   file can't be fetched or shared even if you know its name.

## #46 — Basename resolution (chosen design)

Decision (April): resolve any permanent sound by its **basename**, searching the
permanent tree recursively — the same pattern `resolveAdHocSoundPath` already uses
for the ad-hoc tree. Rationale:

- **No client contract change.** The client keeps sending `<uuid>.wav`; slashes
  never touch the URL, so no `%2F`-in-path proxy pitfalls and no oatpp tail-route
  surgery.
- **DRY.** Reuses the existing recursive-basename-search pattern.
- **Safe.** Dialog filenames are globally-unique UUIDs, so basename collisions
  across subdirs are effectively impossible. The canonical-within-root check
  remains the security boundary.

The human-meaningful identity of these files is supplied by #47 (embedded
provenance), not by the listing label — so #46 just needs them to be listable and
actionable.

### Server changes

- **`SoundService.cpp`**
  - Extract a private helper `findSoundByBasename(root, filename)` that walks a
    root with `fs::recursive_directory_iterator`, returns the canonical path of the
    first regular file whose `filename()` matches, guarded by a
    canonical-within-root check. Returns `std::optional<std::string>`.
  - Rewrite `resolveAdHocSoundPath` to delegate to it (root = ad-hoc root).
  - Add `resolvePermanentSoundPath(filename)`: try the flat `root/filename` fast
    path first (preserves today's behavior for top-level sounds), then fall back to
    `findSoundByBasename(permanentRoot, filename)`.
  - `getAllSounds`: `fs::directory_iterator` → `fs::recursive_directory_iterator`.
    It already emits `filepath.filename()` (basename) as `file_name` and looks up
    sidecar `.txt`/`.json` relative to the file's own directory, so recursion is the
    only change needed.
- **`SoundController.h`**
  - `getSound`: replace the inline flat-path resolution with
    `m_soundService.resolvePermanentSoundPath(...)`.
  - `getShareableSound`: replace the inline permanent-store `canonical(root + "/" +
    name)` block with `resolvePermanentSoundPath(...)` (still falling through to the
    ad-hoc store on miss).
  - `sanitizeFilename` is unchanged — basenames only, still rejects `/`.

### Tests

- `SoundService` finds a file planted in a `dialog/` subdir by basename.
- `getAllSounds` lists a file planted in `dialog/`.
- Flat top-level sounds still resolve (no regression).
- Traversal (`../`) still rejected.

## #47 — Embedded provenance in dialog WAVs

Goal: a dialog WAV in `dialog/` should carry, in-file, enough to answer "what
script is this?" — script id, generation id(s), the full script **text**, and which
creature is on which of the 17 channels. Point-in-time snapshot semantics, mirroring
the existing `animation.metadata.source_script_turns` (a copy, not a live pointer).

### Format

Append an **`iXML`** chunk (the pro-audio standard; RIFF chunk id `iXML`) *after*
the `data` chunk. Verified safe against every server-side reader:

- `WavFileReader::readWavInfo` walks chunks and honors the declared `data` size.
- `MonoWavDownmixer::loadWavAsMono` and `AudioStreamBuffer::loadWaveFile` both use
  `SDL_LoadWAV`, a real chunk parser that reads exactly the `data` chunk.

None assume a 44-byte header or read to EOF, so a trailing `iXML` chunk is ignored
by playback and downmix. The RIFF size field is bumped to cover the new chunk;
`data` size is unchanged.

iXML payload (subset of the spec, plus a private block):

```xml
<?xml version="1.0" encoding="UTF-8"?>
<BWFXML>
  <IXML_VERSION>1.5</IXML_VERSION>
  <PROJECT>creature-server</PROJECT>
  <NOTE>Dialog render</NOTE>
  <TRACK_LIST>
    <TRACK_COUNT>17</TRACK_COUNT>
    <TRACK><CHANNEL_INDEX>1</CHANNEL_INDEX><NAME>Beaky</NAME><INTERLEAVE_INDEX>1</INTERLEAVE_INDEX></TRACK>
    ...
    <TRACK><CHANNEL_INDEX>17</CHANNEL_INDEX><NAME>BGM</NAME><INTERLEAVE_INDEX>17</INTERLEAVE_INDEX></TRACK>
  </TRACK_LIST>
  <USER>
    <SOURCE_SCRIPT_ID>...</SOURCE_SCRIPT_ID>
    <GENERATION_IDS>id1,id2,...</GENERATION_IDS>
    <DIALOG_SCRIPT>full script text, turn by turn</DIALOG_SCRIPT>
  </USER>
</BWFXML>
```

### Server changes (sketch — refined during implementation)

- New `DialogWavMetadata` struct (script id, generation ids, ordered
  `(channel, creatureName)` list, rendered script text) built in `JobWorker` where
  all of this is already in scope at write time.
- `writeDialogWav` gains an optional `const DialogWavMetadata*` param; when present
  it appends the `iXML` chunk. Chunk builder + XML escaping live in a small helper
  (unit-tested independently). Only the permanent-render call site passes metadata;
  ad-hoc/preview WAVs stay lean.
- Mirror the same provenance into the shareable **Ogg** via OpusTags/Vorbis comments
  on the `getShareableSound` path (best-effort; music/non-dialog sounds have none).
- Optional read-back: a tiny `iXML`-extractor + a CLI `sounds provenance <file>` so
  the anonymous-UUID problem is answerable from the terminal.

### Tests

- iXML chunk builder escapes XML metacharacters; round-trips the fields.
- A WAV with an appended `iXML` chunk still loads via `readWavInfo` and `SDL_LoadWAV`
  with identical audio (byte-for-byte data chunk).
- Track list names line up with channel assignments.

## Sequencing

1. #46 first (server): resolver + recursive listing + tests. Foundation — the files
   must be reachable before provenance matters.
2. #47 (server): iXML writer + wire into permanent render + Ogg mirror + tests.
3. Version bump (`VERSION.txt`), Linux/Docker product build check, deploy.
4. Close #46 and #47 on merge to `main`.
