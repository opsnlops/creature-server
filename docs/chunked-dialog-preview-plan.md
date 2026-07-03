# Chunked Dialog Previews — Plan (issue #40)

Long scenes (> ~1800 chars total) fail to preview with "exceeds per-call cap of 2000
(chunkTurns first?)". The cap is ElevenLabs' real per-call ceiling and cannot be raised;
the render path already handles long scenes by splitting at turn boundaries
(`chunkTurns`, per-chunk cache keys, `INTER_CHUNK_GAP_SECS` seams). Preview must do the
same instead of sending the whole scene in one call.

## Design

`DialogPreviewController::loadOrGenerate`, fresh-generation branch:

1. `chunkTurns(inputs)` (422 with the chunker's message if a single turn exceeds the
   per-chunk cap — that's a fatal authoring error today too).
2. Single chunk → existing code path, unchanged.
3. Multiple chunks — mirror the render's per-chunk loop:
   - per-chunk `computeCacheKey(chunk)`; reuse the latest cached generation unless
     `regenerate` was requested; otherwise `generateDialog` + `forcedAlignment` and
     `saveGeneration` under the chunk key — **the same cache entries the render job
     reads**, so render-after-preview reuses the auditioned audio instead of paying
     ElevenLabs twice.
   - merge the chunk generations into ONE whole-scene `CachedGeneration` and save it
     under the scene cache key, so the panel's audio URL / take picker / Ogg share
     keep working unmodified. `cached` = true only when every chunk was a cache hit.

## Merge helper (new, unit-tested)

`src/server/voice/DialogPreviewAssembly.{h,cpp}`:
`mergeChunkGenerations(chunkGens, turnCounts, gapSecs, sampleRate) -> CachedGeneration`
- audio: concat chunk PCM with `INTER_CHUNK_GAP_SECS` (0.30 s) of silence at each seam
  — the same seam pause the render uses, so the audition sounds like the show will
- forced alignment: word/char times offset by each chunk's start time (including gaps);
  a synthetic space char spans each seam so the character stream matches the
  whole-scene transcript convention (turns joined with single spaces)
- voice segments: `dialogInputIndex` offset by turns-before-chunk; character indices
  offset by the prior chunks' character-array lengths (+1 per joining space)
- `loss`: the max across chunks (worst-chunk signal, conservative)

## DRY extraction

The generate → align → save-to-cache block currently exists in the preview single-call
path and the render's chunk loop, and the chunked preview would be a third copy. Extract
`voice::generateChunkWithAlignment(client, apiKey, chunk, cacheKey, span) ->
Result<CachedGeneration>` and use it from all three call sites (refs the standing DRY
audit, #34).

## Not changing

- The 2000-char per-call guard in DialogClient (it mirrors the upstream API).
- The per-chunk single-turn cap (1800 chars) — a single turn that long is an authoring
  error the API would reject anyway; the error message stays clear.
- The console: same endpoints, same DTO shapes; no client work needed.

## Version

3.22.0 (feature — long-scene previews), stacked on the shareable-sounds branches.
