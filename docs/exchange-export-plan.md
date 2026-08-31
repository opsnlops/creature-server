# Streamed Ad-Hoc Exchange Export Plan (issue #150)

Export a whole streamed ad-hoc session ("exchange") — everything Beaky said in one
creature-agent-driven conversation turn — as a single, fully-tagged MP3. Companion
console issues: creature-console#80 (GUI right-click), creature-console#81 (CLI).

## Background

The streaming pipeline (`/api/v1/animation/ad-hoc-stream/{start,text,finish}`)
renders each sentence to its own 17-channel WAV in a per-session directory
(`$TMPDIR/creature-adhoc/<session_id>/s1.wav … sN.wav` + `transcript.txt`) and
chains playback through the cooperative scheduler's in-memory queue. Nothing
durable records the exchange; `sN.wav` basenames aren't globally unique; ad-hoc
WAVs carry no iXML.

Two facts make the design simple:

1. `StreamingAdHocSession::finish()` joins the playback thread, which `.get()`s
   every sentence future — **all part WAVs exist before `/finish` returns**.
   Stitching happens synchronously inside finish; no async "rendering" state is
   observable through the API except a session that hasn't finished yet.
2. Every part is the same format (48 kHz / S16 / 17-ch, per the audio contract)
   and playback is gapless, so plain data-chunk concatenation reproduces exactly
   what was heard.

## Design

### New resource: `adhoc_exchanges` collection

One document per streaming session, TTL'd on `created_at` with the same
`--adhoc-animation-ttl-hours` knob (matches the lifetime of the WAVs it points at).

```jsonc
{
  "session_id":   "<uuid>",            // natural key
  "creature_id":  "…",
  "creature_name": "Beaky",
  "status":       "streaming | ready | partial | failed",
  "title":        "Beaky - 20260820143012 - somebody-is-at-the-door",
  "transcript":   "full text",
  "sound_file":   "/tmp/creature-adhoc/<sid>/<sid>.wav",  // absolute (AdHoc bucket rule)
  "duration_ms":  23417,
  "finished_at":  <epoch ms>,
  "parts": [ { "index": 1, "animation_id": "…", "text": "…", "duration_ms": 3120 } ]
  // + BSON created_at date prepended at insert for the TTL index (same as adhoc_animations)
}
```

Lifecycle: inserted at `/start` with `status: "streaming"` (so the list answers
"what is Beaky saying right now"); finalized inside `/finish` after the stitch —
`ready` when every sentence rendered, `partial` when some failed but ≥1 landed,
`failed` when none did. A session whose agent dies before `/finish` stays
`streaming` until the TTL reaps it (same as its orphaned directory today).

### Stitched WAV

At finish: concatenate the data chunks of `s1..sN.wav` (skipping failed parts)
into `<session_id>.wav` in the session directory, then append an iXML chunk:

- `title` — `"<creature> - <timestamp> - <slug of full text>"` (also the doc title)
- `script` — one `DialogScriptLine` per sentence, speaker = creature name
  (→ ARTIST + LYRICS in the MP3 via the #148 tag mapping)
- `tracks` — the creature's channel lane
- `fileUid` — the session id; `take` — `"exchange"`
- `generationIds` — the ElevenLabs request ids, harvested from the session's
  existing `requestIdFutures_` (already tracked for prosody chaining; all
  resolved by finish time, failures resolve to empty string and are skipped)

Multichannel iXML support: a new `stitchMultichannelWavs(parts, outPath,
provenance)` in `PcmWavWriter` — reads each part's `data` chunk, writes one
header with the summed size, appends `makeIxmlChunk(buildIxml(prov, 17))`, and
folds the chunk into the RIFF size (same technique as `wrapMonoPcmAsWav`).
Per-part durations fall out of the data-chunk sizes for free.

### Endpoints (all on `StreamingAdHocController`)

```
GET api/v1/animation/ad-hoc-stream/exchanges                        → list, newest first
GET api/v1/animation/ad-hoc-stream/exchange/{sessionId}             → full neutral exchange JSON
GET api/v1/animation/ad-hoc-stream/exchange/{sessionId}/audio.mp3   → SoundRenditionService::renderWav(stitched)
GET api/v1/animation/ad-hoc-stream/exchange/{sessionId}/audio.ogg   → same, Ogg/Opus
GET api/v1/animation/ad-hoc-stream/exchange/{sessionId}/audio.wav   → raw stitched 17-ch WAV
```

- `sessionId` is validated as a UUID (charset + shape) before any filesystem or
  DB use — it becomes part of an on-disk path.
- Audio while `status == "streaming"` → `409` + `Retry-After: 5`; unknown
  session → `404`; `partial`/`ready` both serve audio.
- `Cache-Control: public, max-age=31536000, immutable` — UUID-addressed content
  that can never change once finalized, and the deterministic encoders keep
  repeated renders byte-identical. (Deliberately different from the `no-store`
  on basename-addressed ad-hoc renditions, which *can* re-resolve elsewhere.)
- `Content-Disposition: attachment; filename="<title>.<ext>"` (quoted, sanitized).

`/finish` response gains additive fields: `animation_id` becomes the real last
animation id, plus `exchange_status`, `parts_rendered`. Old clients decode
around them harmlessly (Swift DTOs use explicit CodingKeys).

### Cache invalidation

New `CacheType::AdHocExchangeList` ↔ `"ad-hoc-exchange-list"`. Fired (via a
`storage::` publisher pairing, per the issue-#11 rule) on insert at `/start`
and on finalize at `/finish`.

## Work list

1. **Model** — `src/model/AdHocExchange.{h,cpp}`: structs (`AdHocExchange`,
   `AdHocExchangePart`) and strict persistence JSON codec. Status as a string
   enum with helpers. Public API serialization is kept separately in
   `src/api/StreamingAdHocContracts.h`.
2. **DB** — `ADHOC_EXCHANGES_COLLECTION` in `config.h`;
   `src/server/animation/exchange.cpp` (existing globbed dir — no CMake edit):
   `insertAdHocExchange`, `finalizeAdHocExchange`, `listAdHocExchanges`,
   `getAdHocExchange`, `ensureAdHocExchangeIndexes` (TTL, mirrors the ad-hoc
   animation index); declarations in `database.h`; ensure-indexes call next to
   the existing one in `main.cpp`.
3. **Cache type** — `CacheType::AdHocExchangeList` + `CacheInvalidation.cpp`
   both directions.
4. **Storage facade** — `publishAdHocExchange` / `finalizeAdHocExchange`
   pairing DB write + invalidation.
5. **Stitcher** — `stitchMultichannelWavs` in `PcmWavWriter` (+ a small RIFF
   `data`-chunk locator).
6. **Session** — track per-sentence text/outcome (playback thread already sees
   each result); create the doc in `start()`; stitch + finalize in `finish()`;
   return a richer finish result (last animation id, counts, status).
7. **Controller + contracts** — five endpoints, neutral exchange and
   finish-response JSON, UUID validation.
8. **Tests** — stitcher (sample counts, channel placement, iXML round-trip
   through `readIxmlChunk`/`parseIxmlProvenance`); rendition of a stitched WAV
   carries TITLE/ARTIST/LYRICS; exchange JSON codec round-trip; UUID validator.
9. **Version/format** — `VERSION.txt` → 3.44.0; clang-format; full test run.

## Out of scope

- The one-shot `/animation/ad-hoc` fallback path (no grouping exists there).
- Promoting an exchange past the TTL into the permanent sound store (follow-up).
- Stale-session sweep for agents that never call `/finish` (pre-existing).
