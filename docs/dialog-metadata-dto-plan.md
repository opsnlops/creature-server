# Dialog metadata on the Sound DTO — Implementation Plan (issue #56)

Expose the treasure the dialog render pipeline already embeds in every WAV (iXML: script,
tracks, mouth cues) as **structured JSON on the existing `SoundDto`**, populated "when we
have it" — present iff the file carries the embedded data, empty for plain sounds and old
renders. Word-level timings get a field now and fill in once the render pipeline persists
them (Part 2, below).

Delivery decision (per April): enrich the **existing `SoundDto`** rather than adding a
separate `/metadata` endpoint. The DTO is already built in one place — the sound-list
builder (`SoundService::getAllSounds`, `SoundService.cpp:147-164`) — which already reads the
iXML cheaply (it seeks past the audio `data` chunk). We extend that same parse.

## New DTOs (in `src/model/Sound.h`, alongside `SoundDto`)

```
DialogTurnDto      { String speaker; String line; }
SoundTrackDto      { UInt16 channel; String creature_name; }
MouthCueDto        { Float64 start_s; Float64 end_s; String shape; }
TrackMouthCuesDto  { UInt16 channel; String creature_name; List<Object<MouthCueDto>> cues; }
WordTimingDto      { String word; Float64 start_s; Float64 end_s; }
TrackWordsDto      { UInt16 channel; String creature_name; List<Object<WordTimingDto>> words; }
```

## New `SoundDto` fields (all optional — backward compatible)

- `List<Object<DialogTurnDto>>      script_turns`  — structured turns (vs. the existing `script` blob)
- `List<Object<SoundTrackDto>>      tracks`        — channel → creature name (creature lanes + BGM)
- `List<Object<TrackMouthCuesDto>>  mouth_cues`    — per-track Rhubarb visemes, already embedded
- `List<Object<TrackWordsDto>>      word_timings`  — per-track word timing; empty until Part 2

The existing scalar fields (`title`, `source_script_id`, `script`, `generation_ids`,
`has_embedded_script`, `has_embedded_lipsync`) stay untouched — this is purely additive.

## Parser (extend `src/server/voice/IxmlReader.{h,cpp}`)

Today's `extractIxmlField` is a flat first-match extractor — it can't handle the repeated
`<TRACK>` elements or the nested `<LIPSYNC>` block. Add small structured parsers that return
the **same structs the writer already uses** (`IxmlWriter.h`), for clean reader/writer
symmetry:

- `parseIxmlTrackList(ixml)  -> std::vector<DialogTrackInfo>`   (from `<TRACK_LIST>`)
- `parseIxmlLipsync(ixml)    -> std::vector<DialogLipsyncTrack>` (from `<USER><LIPSYNC>`, unpacking the packed `"start end shape;..."` `<CUES>` string into `DialogLipsyncCue`s)
- `parseDialogScriptTurns(blob) -> std::vector<DialogScriptLine>` (best-effort split of the `DIALOG_SCRIPT` blob: split on `\n`, then first `": "` → speaker/line. Lossy only if a line body itself contains a newline — acceptable for old renders; see Part 2 note.)
- `parseIxmlWordAlignment(ixml) -> std::vector<DialogWordTrack>` (present-when-embedded; see Part 2)

Implementation helper: a `forEachElement(section, "TRACK", fn)` that walks repeated
`<TRACK>…</TRACK>` spans within a parent substring, reusing `extractIxmlField` for the leaf
values inside each.

`Sound.cpp::convertSoundToDto` maps these structs → the DTOs above. `SoundService` calls the
parsers right where it already calls `readIxmlChunk`.

## Part 2 (DONE) — persist word-level alignment at render time

Implemented. The ElevenLabs `/v1/forced-alignment` words (already produced per chunk) are now
carried through the pipeline instead of being dropped after slice-boundary use:

- `DialogPerCreature` gained a `words` vector (`DialogWordTiming`, seconds on the tightened
  timeline). `assembleChunk` captures each turn's FA words and, in Step 3, shifts them by the
  **exact same `(pos - origStart)/SR`** as the chars (a shared `shiftSec`); `concatChunks`
  offsets them by the same per-chunk `offset/SR` as the mouth timings. So words line up with
  both the audio and the existing mouth cues.
- `DialogWavProvenance` gained `wordAlignment` (`std::vector<DialogWordTrack>`); `JobWorker`
  populates it beside the lipsync build, using the same voice→lane mapping. No `DialogCache`
  change — the cached provenance never carried the heavy per-creature data; it's rebuilt each
  render from `assembled`.
- `buildDialogIxml` emits a `<WORD_ALIGNMENT>` block (per-track `<WORDS>start end word;…</WORDS>`,
  `;` in tokens sanitized), and `parseIxmlWordAlignment` reads it back into the DTO's
  `word_timings`.

Old renders degrade gracefully (script + tracks + cues, no words); a re-render upgrades them.
Structured turns could also be emitted directly in iXML in a future pass, retiring the
best-effort `DIALOG_SCRIPT` blob split.

## Tests (`tests/server/voice/IxmlReader_test.cpp`, extend)

- Round-trip `buildDialogIxml` → parse tracks back (complete 17-ch list, names by channel).
- Round-trip lipsync: build with cues → `parseIxmlLipsync` returns the same cues (start/end/shape).
- `parseDialogScriptTurns` splits `"Beaky: hi\nPip: bye"` → two turns; handles a `": "` inside a line body (splits on first only).
- Empty / no-iXML / plain-sound → empty vectors, no throw.

## Notes

- List weight: the structured arrays ride the existing `GET /api/v1/sound` list. If that
  grows too heavy, moving the per-track cue/word arrays behind a per-sound fetch is a
  localized change (populate in a single-sound builder instead of the list loop).
- Keep the raw iXML in the WAV and the raw-iXML provenance endpoint as-is — this DTO is a
  *view*, per the issue.
