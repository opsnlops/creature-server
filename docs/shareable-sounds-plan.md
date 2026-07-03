# Shareable Sounds — Server Implementation Plan (issue #36)

Turn any stored sound into a small, shareable Ogg/Opus file on demand. Companion client
work: creature-console#9.

## Endpoint

`GET /api/v1/sound/shareable/{filename}` (SoundController)

1. Sanitize `{filename}` with the existing `SoundController::sanitizeFilename`.
2. Resolve the WAV: try the permanent store (`storage::resolveSoundPath` + canonical
   containment check, as `getSound` does), then the ad-hoc store
   (`SoundService::resolveAdHocSoundPath`). 404 if neither has it.
3. Load + downmix with the existing `creatures::audio::loadWavAsMono` — **do not write a
   second downmixer**. It handles 1/2/17-channel S16 WAVs and preserves the sample rate.
4. Encode mono S16 → Ogg/Opus at **96 kbps** (named constant), **48 kHz required** — the
   whole pipeline is 48 kHz by design, so no resampling and no other rates: a non-48k
   file in the stores is a smell, and it gets `InvalidData` with a clear message rather
   than papering over. (Requiring 48k also keeps the Ogg muxer's pre-skip/granule
   bookkeeping trivial, since Opus granule positions are defined in 48 kHz units.)
5. Return the whole buffer via the house pattern (`ResponseFactory::createResponse`,
   `Content-Type: audio/ogg` — already in `getMimeType` — and
   `Content-Disposition: attachment; filename="<basename>.ogg"`, like `getAdHocSound`).

No server-side caching of the encoded result: Opus encodes these clips in well under a
second, and a cache would just be another thing to invalidate.

## Encoder

New `src/server/audio/OggOpusWriter.{h,cpp}` (globbed dir — no CMake source edit):

```cpp
namespace creatures::audio {
// Encode mono S16 PCM into a complete Ogg/Opus file in memory. 96 kbps default.
Result<std::vector<uint8_t>> encodeMonoToOggOpus(const std::vector<int16_t> &samples,
                                                 int sampleRate, int bitrate = kShareableBitrate);
}
```

**Dependency decision** — the repo links core libopus only; there is no Ogg container
support. (AudioCache's "OggOpus" format is deliberately custom — it's the raw wire
format streamed to clients. It is NOT a broken Ogg implementation and must be left
alone; this feature adds a real Ogg muxer *alongside* it, for downloads only.)
libopusenc was ruled out: upstream is autotools-only (no CMakeLists.txt), so it can't
ride FetchContent like every other dep here. Instead:

- FetchContent `xiph/ogg` (official CMake support) next to the opus block, and hand-mux
  per RFC 7845: OpusHead (pre-skip from `OPUS_GET_LOOKAHEAD`) + OpusTags pages, then
  `ogg_stream` audio pages around `opus_encode` output — 20 ms frames (960 samples),
  final packet padded with silence and the end granule position set to trim it.

Either way the CMakeLists edit busts the Docker Phase-1 dep cache once (expected; same as
any dep addition).

## Tests

`tests/server/audio/OggOpusWriter_test.cpp` (added to the explicit test-source list, and
`opus`/`ogg` linked into `creature-server-test`, which currently links neither):
- encodes a sine-wave buffer and validates the output starts with an Ogg page (`OggS`),
  contains an `OpusHead` capture pattern, and is non-trivially smaller than the input
- rejects unsupported sample rates
- empty input → InvalidData

Endpoint behavior (sanitization, both-store resolution) follows the patterns already
covered by existing controller conventions; verified manually via curl + the client.

## Versioning

Bump `VERSION.txt` (deployable change) when the endpoint lands.
