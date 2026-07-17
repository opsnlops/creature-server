# MP3 Rendition Endpoint — Server Implementation Plan (issue #57)

Turn any stored sound into a small, **widely-playable** MP3 on demand — the same trick as
the shareable Ogg/Opus endpoint (#36), but in the one format that plays *everywhere* the
Ogg one doesn't: AVFoundation (Console in-app + progressive streaming), Slack inline, and
every browser. Companion client work: creature-console#32 (already wired to
`getSoundMp3URL(_:)`).

## Endpoint

`GET /api/v1/sound/mp3/{filename}` (SoundController) → `audio/mpeg`

This is `getShareableSound` with the final encode swapped. Everything up to the encode is
reused verbatim:

1. Sanitize `{filename}` with `SoundController::sanitizeFilename` (403 on bad names).
2. Resolve the WAV: permanent store first (`resolvePermanentSoundPath`, which does the
   top-level-then-recursive-basename search so `dialog/` renders resolve — #46), then the
   ad-hoc store (`resolveAdHocSoundPath`). 404 if neither has it.
3. Load + downmix with `creatures::audio::loadWavAsMono` — **do not write a second
   downmixer**. Handles 1/2/17-channel S16 WAVs, preserves the sample rate.
4. **New:** encode mono S16 → MP3 with `encodeMonoToMp3(samples, sampleRate, bitrate)`.
5. Return the whole buffer via the house pattern (`ResponseFactory::createResponse`,
   `Content-Type: audio/mpeg`, `Content-Disposition: attachment; filename="<basename>.mp3"`).

Same status shapes as shareable: **403** bad filename, **404** not found, **422** un-decodable
/ wrong-rate WAV, **500** encoder failure. No server-side caching of the encoded result —
LAME encodes these clips in well under a second, and a cache is just another thing to
invalidate.

## Encoder

New `src/server/audio/Mp3Writer.{h,cpp}` (globbed dir — no CMake source edit for the main
executable; the test target lists sources explicitly, so it does need one line):

```cpp
namespace creatures::audio {
// Optional ID3v2 TXXX frames — mirrors OggComments; used for dialog provenance parity (#47).
using Id3Comments = std::vector<std::pair<std::string, std::string>>;

inline constexpr int kShareableMp3Bitrate = 128000; // 128 kbps CBR mono, matches the Ogg target
// (kShareableSampleRate = 48000, reused from OggOpusWriter.h)

Result<std::vector<uint8_t>> encodeMonoToMp3(const std::vector<int16_t> &samples, int sampleRate,
                                             int bitrate = kShareableMp3Bitrate,
                                             const Id3Comments &comments = {});
}
```

- **48 kHz required**, same as the Ogg encoder: the whole pipeline is 48 kHz by design, so a
  non-48k file in the stores is a smell — reject with `InvalidData` (→ 422) rather than
  silently resample. LAME encodes 48 kHz as MPEG-1 Layer III; Slack/AVFoundation/browsers
  all handle it.
- **128 kbps CBR mono** (`vbr_off`, `lame_set_brate(128)`), quality 2. CBR keeps duration
  trivially computable for progressive players and keeps output deterministic.
- **No Xing/Info (VBR) header** on the first cut (`lame_set_bWriteVbrTag(0)`): writing it
  in-memory means seeking back to overwrite the reserved first frame, and CBR players
  compute duration fine without it. Deterministic output falls out for free (mirrors the
  byte-stable Ogg encoder → same testability). Revisit if a player wants gapless.

### Dependency decision — system `libmp3lame`, not FetchContent

libopus/libogg rode FetchContent because they ship first-class CMake builds. **LAME does
not** — upstream is autotools/nmake only, so there's nothing to `FetchContent_Declare`
cleanly. But the repo already links *system* audio libraries the same way LAME wants to be
linked: `find_package(SDL2_mixer)` + apt in the Docker images. libmp3lame follows that
exact precedent:

- **Build image** (`Dockerfile` build stage): add `libmp3lame-dev`.
- **Runtime image** (`Dockerfile` runtime stage): add `libmp3lame0`.
- **CMake**: `pkg_check_modules(MP3LAME REQUIRED lame)` (falls back to `find_library(mp3lame)`
  if no `.pc`), link `mp3lame` into `creature-server` and `creature-server-test`.
- **The `.deb` picks up the runtime `Depends:` for free** — `cmake/Package.cmake` already
  sets `CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON`, so `dpkg-shlibdeps` auto-adds `libmp3lame0`
  from the linked shared lib. Same mechanism that already pulls in `libsdl2-mixer`.

This keeps the encoder wrapper hand-rolled (parity with `OggOpusWriter`) while sourcing the
codec the way this repo already sources its other system audio codecs.

## Provenance parity (issue #57 nice-to-have, #47 lineage)

Mirror embedded dialog provenance (iXML) into **ID3v2 TXXX** frames, exactly as the Ogg
endpoint mirrors it into OpusTags. Reuse `voice::readIxmlChunk` + `extractIxmlField`, map the
same fields (`TITLE`, `SOURCE_SCRIPT_ID`, `DESCRIPTION` ← `DIALOG_SCRIPT`), and prepend a
minimal **ID3v2.4.0** tag with one `TXXX` (user-defined text, UTF-8 encoding byte `0x03`,
`key\0value` payload) per field. Non-WAV sources and WAVs without an iXML chunk contribute
no frames and get a bare MP3 stream. Kept behind the optional `comments` arg so the encoder
stays testable without a WAV on disk.

## Tests (`tests/server/audio/Mp3Writer_test.cpp`)

Mirror `OggOpusWriter_test.cpp`:
- Encodes a sine to a plausible MP3 (starts with an MPEG frame sync `0xFF 0xEx` **or** an
  `ID3` tag; dramatically smaller than the raw PCM).
- Deterministic for identical input.
- Rejects non-48k sample rates (`InvalidData`).
- Rejects empty input (`InvalidData`).
- Embeds `TXXX`/key/value bytes when `comments` are supplied.
- Handles input shorter than one MP3 frame (tail flush).

## Follow-ups / notes

- Likely **supersedes the Ogg shareable path** for the "Generate Shareable Version" button
  too, since MP3 is the more useful share format — but that's a client decision, out of
  scope here.
- If some target ever chokes on 48 kHz MP3 (not expected), the fallback is 44.1 kHz — a
  one-line change, but it'd mean resampling, which we deliberately avoid.
